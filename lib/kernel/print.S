TI_GDT  equ  0
RPL0  equ  0
SELECTOR_VIDEO equ (0x0003<<3) + TI_GDT + RPL0

section .data
put_int_buffer dq 0                     ; 定义8字节缓冲区用于数字到字符的转换

[bits 32]
section .text
; ----------------------------------------------------
; put_str 通过put_char来打印以0字符结尾的字符串，输入: 栈中参数为打印的字符串；输出：无
; ----------------------------------------------------

global put_str
put_str:
    ; 由于本函数只用到了ebx和ecx，只备份这两个寄存器即可
    push ebx
    push ecx
    xor ecx, ecx                            ; 准备用ecx传递字符，故而清空ecx
    mov ebx, [esp + 12]                ; 从栈中得到待打印的字符串首地址，存入ebx
.goon:
    mov cl, [ebx]                           ; 获得字符串对应的字符ASCII码，存入cx中低8位
    cmp cl, 0                                ; 比较字符是否为0 (ASCII码值为0对应'/0'说明到达字符结尾)
    jz .str_over
    push ecx                                 ; 为put_char函数传递参数
    call put_char                           ; 调用put_char
    add esp, 4                               ; 回收参数所占的栈空间
    inc ebx                                    ; 使ebx指向下一个字符
    jmp .goon

.str_over:
    pop ecx
    pop ebx
    ret

; ----------------------- put char ----------------------
;功能描述：把栈中的一个字符写入光标所在处
; --------------------------------------------------------
global put_char
put_char:
    pushad                                ; 备份32位寄存器环境
    mov ax, SELECTOR_VIDEO   ; 需要保证gs中为正确的视频段选择子，为保险起见每次打印时都为gs赋值
    mov gs, ax

; ---------------  获取当前光标位置  -------------------
    ; 先获取高8位
    mov dx, 0x03d4                    ; 索引寄存器的端口地址移入dx
    mov al, 0x0e                         ; al存储的值用于提供光标位置的高8位
    out dx, al                              ; 把al存储的值输出到dx指向的端口地址，即CRT Controller Data Register的Address Register上
    mov dx, 0x03d5                    ; 确定了要操作的寄存器是Cursor Location High Register后，通过读写数据端口0x3d5来获得光标位置
    in al, dx                                ; 从dx指向的端口地址，即CRT Controller Data Register的Data Register上输入进al，得到了光标位置的高8位
    mov ah, al

    ; 再获取低8位
    mov dx, 0x03d4
    mov al, 0x0f
    out dx, al
    mov dx, 0x03d5
    in al, dx

    ; 将光标存入bx
    mov bx, ax
    ; 下面这行是在栈中获取待打印的字符
    mov ecx, [esp + 36]              ; pushad 压入4 x 8 = 32字节, 加上主调函数4字节的返回地址，故esp+36字节
    
    cmp cl, 0xd                           ; CR是0x0d, LF是0x0a
    jz .is_carriage_return
    cmp cl, 0xa
    jz .is_line_feed

    cmp cl, 0x8                           ; BS(backspace)的ASCII值是8
    jz .is_backspace

    jmp .put_other

.is_backspace:
    dec bx                                   ; 将bx减1，这样光标便指向了前一个字符了                                  
    shl bx, 1                                ; 光标左移1位等于乘以2，表示光标对应显存中的偏移字节
    
    mov byte [gs:bx], 0x20           ; 在bx指向的显存低字节处先把空格的ASCII码0x20写入，覆盖掉要删除的字符
    inc bx                                    ; bx指向了该字符的属性位置
    mov byte [gs:bx], 0x07           ; 将属性0x7写入高字节（0x7表示黑屏白字）
    shr bx, 1                                ; 将bx除2取整，因此bx由显存中的相对地址恢复成了光标坐标，此时光标指向新覆盖的空格
    jmp .set_cursor

.put_other:
    ; 处理可见字符
    shl bx, 1                                 ; 光标位置用2字节表示，将光标值乘2表示对应显存中的偏移字节
    mov [gs:bx], cl                        ; ASCII码值本身
    inc bx                                    ; bx+1,成为新的可以打印字符的坐标            
    mov byte [gs:bx], 0x07           ; 将该字符属性写入高字节（0x7表示黑屏白字）
    shr bx, 1                                ; 除以2恢复成老的光标坐标
    inc bx                                    ; 下一个光标位置
    cmp bx, 2000
    jl .set_cursor                          ; 若光标值小于2000, 表示未写到显存的最后，则去设置新的光标值，若超过屏幕字符数大小(2000）则换行处理(即继续向下运行)

.is_line_feed:                             ; 是换行符LF(\n)
.is_carriage_return:                    ; 是回车符CR(\r)
; 若是CR，只要把光标移到行首就行了
    xor dx, dx                             ; dx是被除数的高16位，清0
    mov ax, bx                           ; ax是被除数的低16位
    mov si, 80                             ; 由于是效仿Linux，Linux中\n表示下一行行首，故本系统中把\n和\r都处理为Linux中\n的意思，也就是下一行行首
    div si                                    ; 光标坐标位置除以80的余数便是取整, 其中经过div除法操作后，dx寄存器中为余数, ax寄存器为商
    
    sub bx, dx                            ; 用坐标值bx - 余数dx得到行首坐标，存在bx中，从而实现了回车功能（但目前还没有更新光标坐标寄存器，更新后才算完成）

.is_carriage_return_end:            ; 回车符CR处理结束

    add bx, 80                            ; 若是LF(\n)，将光标移+80便可
    cmp bx, 2000

.is_line_feed_end:
    jl .set_cursor

.roll_screen:                             ; 若超过屏幕大小，开始滚屏
    cld
    mov ecx, 960                       ; 要搬运2000-80=1920个字符，共1920*2=3840字节，一次只能搬运4字节，因此共搬运3840/4=960次
    mov esi, 0xc00b80a0            ; 把要复制的起始地址赋给esi寄存器，也就是屏幕第1行的起始地址，物理地址为0xb80a0, 这里用其虚拟地址0xc00b80a0
    mov edi, 0xc00b8000           ;  把目的地址赋给edi寄存器，也就是屏幕第0行的起始地址，物理地址是0xb8000, 同上采用虚拟地址
    rep movsd                           ; rep 配合 movsd开始循环复制，直至把第24行数据复制完毕
    
    ; 此时滚屏操作还差最后一步，需要将最后一行用空格填充
    mov ebx, 3840                     ; 最后一行首字符在显存上的第一个字节偏移地址 = 1920 * 2
    mov ecx, 80                         ; 一行是80个字符，每次清空1字符, 故清空一行需要80次

.cls:
    mov word [gs:ebx], 0x0720   ; 0x0720是黑底白字的空格键(07属性，20空格，注意小端序)
    add ebx, 2
    loop .cls
    
    mov bx, 1920                       ; 将光标重置为1920，最后一行的首字符

; 将光标设为bx值
.set_cursor:
    ; (1) 先设置高8位
    mov dx, 0x03d4                   ; 索引寄存器
    mov al, 0x0e                         ; 提供光标位置的高8位
    out dx, al
    mov dx, 0x03d5                   ; 通过读写数据端口0x3d5来设置光标位置
    mov al, bh
    out dx, al
  
    ;（2）再设置低8位
    mov dx, 0x03d4
    mov al, 0x0f
    out dx, al
    mov dx, 0x03d5
    mov al, bl
    out dx, al

.put_char_done:
    popad
    ret

global cls_screen
cls_screen:
    pushad
    ; 由于用户程序的CPL为3, 显存段的DPL为0, 故用于显存段的选择子gs在低于自己特权的环境中必须为0
    ; 从而导致用户程序再次进入中断后, gs为0，因此直接在put_str中每次都为gs赋值
    mov ax, SELECTOR_VIDEO
    mov gs, ax

    mov ebx, 0
    mov ecx, 80 * 25
.cls:
   mov word [gs:ebx], 0x0720		  ;0x0720是黑底白字的空格键
   add ebx, 2
   loop .cls 
   mov ebx, 0

 .set_cursor:				  ;直接把set_cursor搬过来用,省事
;;;;;;; 1 先设置高8位 ;;;;;;;;
   mov dx, 0x03d4			  ;索引寄存器
   mov al, 0x0e				  ;用于提供光标位置的高8位
   out dx, al
   mov dx, 0x03d5			  ;通过读写数据端口0x3d5来获得或设置光标位置 
   mov al, bh
   out dx, al

;;;;;;; 2 再设置低8位 ;;;;;;;;;
   mov dx, 0x03d4
   mov al, 0x0f
   out dx, al
   mov dx, 0x03d5 
   mov al, bl
   out dx, al
   popad
   ret

; -----------------------  将小端字节序的数字编程对应的ASCII后，倒置  ----------------
; 功能描述：将32位整型数字转换成字符后输出
; 输入：栈中参数为待打印的数字
; 输出：在屏幕上打印十六进制数字，并不会打印前缀0x
; 如打印十进制15时，只会直接打印f，不会是0xf
; ---------------------------------------------------------------------------------------
global put_int
put_int:
    pushad                                 ; 将8个寄存器(eax, ecx, edx, ebx, esp, ebp, esi, edi)全压栈
    mov ebp, esp                        ; 将当前栈顶位置esp暂存入ebp中作为访问栈中元素的基址
    mov eax, [ebp + 36]              ; 内存寻址方式获得参数，将其存入eax中
    mov edx, eax
    mov edi, 7                             ; 指定在put_int_buffer中初始的偏移量
    mov ecx, 8                             ; 32位数中，十六进制数字的位数是8个, 循环转换8个字符
    mov ebx, put_int_buffer         ; 将缓冲区基址存入ebx
    
    ; 将32位数字按照十六进制的形式从低位到高位逐个处理，一共要处理8个十六进制数字
.16based_4bits:
    ; 遍历每一位十六进制数字
    and edx, 0x0000000F             ; 解析十六进制数字的每一位，and与操作后，edx只有低4位有效
    cmp edx, 9                            ; 判断数字是否大于9
    jg .is_A2F                               ; 大于9则说明该十六进制数位范围在A~F，跳转
    add edx, '0'                           ; 走到这，说明没有跳转，即该当前十六进制数位在0~9，用该数字加上‘0’的ASCII码可得到该数字对应的字符
    jmp .store

.is_A2F:
    sub edx, 10                             ; A~F减去10所得之差，再加上字符A的ASCII码，便是A~F对应的ASCII码
    add edx, 'A'

;---- 将每一位数字转换成对应的字符后，按照类似“大端”的顺序存储到缓冲区put_int_buffer
.store:
    mov [ebx + edi], dl                 ; 将edx的低8位（即ASCII码）存入到指定的位置，也就是低位字符存入高内存地址处
    dec edi
    shr eax, 4                                ; 去掉已转换的低4位十六进制数位，开始下一轮转换
    mov edx, eax
    loop .16based_4bits

; ------------- 现如今，put_int_buffer中已全是字符，打印之前需要把高位连续的字符去掉，比如字符0000123变成123  ----------
.ready_to_print:
    inc edi                                    ; edi由-1变为0

.skip_prefix_0:
    cmp edi, 8                              ; 若已经比较第9个字符了，则表示待打印的字符串为全0
    je .full0

; 找出连续的0字符，edi作为非0的最高位字符的偏移
.go_on_skip:
    mov cl, [put_int_buffer + edi]
    inc edi
    cmp cl, '0'
    je .skip_prefix_0                      ; 若当前字符为‘0’，跳回skip_prefix_0继续判断下一位字符是否为0
    dec edi                                   ; 为判断下一位高位字符是否为0，edi在上面的inc操作中指向了下一个字符，这里要减回来从而令edi继续指向当前字符
    jmp .put_each_num                

.full0:
    mov cl, '0'                               ; 输入的数字为全0时，则只打印0

.put_each_num:
    push ecx                                  ; 此时cl中为可打印的字符
    call put_char                            
    add esp, 4                                ; 回收参数所占的栈空间
    inc edi                                      ; 使edi指向下一个字符
    mov cl, [put_int_buffer + edi]    ; 获取下一个字符到cl寄存器
    cmp edi, 8
    jl .put_each_num
    popad
    ret

global set_cursor
set_cursor:
   pushad
   mov bx, [esp+36]
;;;;;;; 1 先设置高8位 ;;;;;;;;
   mov dx, 0x03d4			  ;索引寄存器
   mov al, 0x0e				  ;用于提供光标位置的高8位
   out dx, al
   mov dx, 0x03d5			  ;通过读写数据端口0x3d5来获得或设置光标位置 
   mov al, bh
   out dx, al

;;;;;;; 2 再设置低8位 ;;;;;;;;;
   mov dx, 0x03d4
   mov al, 0x0f
   out dx, al
   mov dx, 0x03d5 
   mov al, bl
   out dx, al
   popad
   ret
