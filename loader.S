%include "boot.inc"
SECTION loader vstart=LOADER_BASE_ADDR

LOADER_STACK_TOP equ LOADER_BASE_ADDR
;jmp loader_start
;本行jmp代码取消掉，改由mbr.S中的代码进行跳转到loader_start开始执行指令

; 构建gdt及其内部的描述符
GDT_BASE: dd 0x00000000      ; 正如P156所述，GDT中的第0个段描述符不可用，故第0个段描述符用全0填充
          dd 0x00000000

CODE_DESC: dd 0x0000FFFF                 ;0~15位段界限全1  16~31位段基址全0
           dd DESC_CODE_HIGH4            ;代码段描述符高32位遵从在boot.inc定义好的DESC_CODE_HIGH4

DATA_STACK_DESC: dd 0x0000FFFF           ; 0~15位段界限全1  16~31位段基址全0
                 dd DESC_DATA_HIGH4      ; 数据段(栈段)描述符高32位遵从在boot.inc定义好的DESC_DATA_HIGH4

VIDEO_DESC: dd 0x80000007        ;0xB8000 到0xBFFFF为文字模式显存 B只能在boot.inc中出现定义了
            dd DESC_VIDEO_HIGH4  ;0x0007 (bFFFF-b8000)/4k = 0x7


GDT_SIZE  equ  $ - GDT_BASE      ;获得GDT大小
GDT_LIMIT equ  GDT_SIZE - 1      ;获得GDT中的最大偏移量，为加载GDT作准备

times 60 dq 0                    ;此处预留60个描述符的空位: 以后还要往GDT中继续塞IDT和TSS描述符。

; 构建代码段、数据段、显存段的选择子
SELECTOR_CODE equ (0x0001<<3) + TI_GDT + RPL0
SELECTOR_DATA equ (0x0002<<3) + TI_GDT + RPL0
SELECTOR_VIDEO equ (0x0003<<3) + TI_GDT + RPL0

; total_mem_bytes用于保存内存容量(以字节为单位),当前偏移loader.bin文件开头地址0x200字节，而loader.bin的加载地址是0x900,因此计算可得该total_mem_bytes地址为0xb00
;(其实我在bochs.disk文件中设置的内存容量是512MB，最后检验的时候会发现total_mem_bytes为0x2000000,为32MB，其实将其左移一位便是512MB，只是我用了16位来表示)
total_mem_bytes dd 0


;以下是gdt的指针(符号地址)，前2字节是gdt界限，后4字节是gdt起始地址 
gdt_ptr dw GDT_LIMIT
        dd GDT_BASE

; 搞出一个连续存储多个ards结构的缓冲区，并人工对齐: total_mem_bytes(4)+gdt_ptr(6)+ards_buf(244)+ards_nr(2), 共256字节
ards_buf times 244 db 0
ards_nr dw 0                ;用于记录ARDS结构体数量

; 定义个字符串来显示咱们要进入保护模式了(其实这段字符串还是在实模式下打印的)
;loadermsg db '2 loader in real'

;-------------------------------- Loader真正开始执行 -----------------------------
loader_start:

    ; 利用BIOS的0x15中断并调用相应的子功能号获取内存布局，这里int 0x15 eax = 0x0000E820, edx = 0x534D4150
    xor ebx, ebx               ; 第一次调用时将ebx置为0
    mov edx, 0x534d4150        ; edx只赋值一次, 循环体中不会改变，用于签名校验
    mov di, ards_buf           ; ards结构缓冲区

.e820_mem_get_loop:
    mov eax, 0x0000e820        ; 因为执行完0x15中断后，eax值会变为0x534d4150,故每次循环都要初始化子功能号
    mov ecx, 20                ; ARDS地址范围描述符结构大小为20字节
    int 0x15
    jc .e820_failed_so_try_e801 ; jc跳转条件 就是CF位=1 carry flag = 1 中途失败了即跳转, 若cF为1表示有错误发生，尝试0xe801
    
    add di, cx                 ; 使di增加20字节指向缓冲区中新的ARDS结构位置,以便下个信息放到新的位置，方便后续遍历
    inc word [ards_nr]         ; 记录ARDS的数量
    cmp ebx, 0                 ; 若ebx为0(此时cf = 0)，这说明ards全部返回,即后面没有ARDS地址范围描述符了，当前已是最后一个
    jnz .e820_mem_get_loop     ; 那么如果ebx确实为0了, 那么cmp ebx, 0执行后ZF为0，循环结束

    ;--- 在所有ards结构中找出最大的内存块(base_add_low + length_low为内存的容量) ---
    mov cx, [ards_nr]          ; 设定循环的次数
    
    mov ebx, ards_buf          ; 遍历每一个ARDS结构体
    xor edx, edx               ; edx存储最大的内存容量，在此先清0

.find_max_mem_area:
;无需判断type是否为1，最大的内存块是一定可被使用的
    mov eax, [ebx]             ; base_add_low:基地址
    add eax, [ebx+8]           ; length_low:内存长度
    add ebx, 20                ; 指向缓冲区中下一个ARDS结构
    
    ;冒泡排序，找出最大的内存块，edx寄存器始终存放最大的内存容量
    cmp edx, eax               ; 根据edx - eax的结果设置标志位
    jge .next_ards             ; eax < edx时，转移到.next_ards执行
    mov edx, eax               ; 否则更新edx值

.next_ards:
    loop .find_max_mem_area    ; 循环执行继续遍历下一个ards，cx-1
    jmp .mem_get_ok            ; 将最大值赋值给之前预定义好的[total_mem_bytes]位置

; --------------------- int 0x15 ax = 0xE801子功能号获取内存大小，但最大支持4G ----------------
; 返回后ax，cx值相同，以KB为单位，bx和dx值相同，以64KB为单位，ax和cx寄存器中为低16MB，bx和dx寄存器为16MB到4GB

.e820_failed_so_try_e801:
    mov ax, 0xe801
    int 0x15
    jc .e801_failed_so_try88   ; 若当前e801方法失败就尝试0x88方法 

;Step1: 先算出低15MB的内存
    mov cx, 0x400              ; cx和ax值相同，cx用作乘数
    mul cx                     ; 相乘后，低16位放在ax 高16位放在了dx
    shl edx, 16                ; edx左移16位，使之位于高16位
    and eax, 0x0000FFFF        ; 把除了16位以下的 16位以上的数清零 防止影响
    or edx, eax                ; 组合成功，获得乘积
    add edx, 0x100000          ; 加了1MB内存空缺
    mov esi, edx               ; 先把低15MB的内存容量存入esi寄存器备份
    
;Step2: 再将16MB以上的内存转换为byte为单位,寄存器bx和dx中是以64KB为单位的内存数量
    xor eax, eax
    mov ax, bx
    mov ecx, 0x10000           ; 0x10000为64KB
    mul ecx                    ; 32位乘法，默认被乘数是eax，积为64位，高32位在edx中，低32位在eax中
    
    add esi, eax               ; 由于此方法只能测出4GB以内的内存，故32位eax足够存储了(相乘后edx必定为0，只加eax即可)
    
    mov edx, esi               ; edx的值为总内存大小
    jmp .mem_get_ok

;--------------- int 0x15 ah = 0x88子功能号获取内存大小，只能获取64MB内的容量 --------------
.e801_failed_so_try88:
    ; int 15后, ax存入的是以KB为单位的内存容量
    mov ah, 0x88
    int 0x15
    jc .error_hlt
    and eax, 0x0000FFFF
    
    mov cx, 0x400              ; 0x400等于1024，将ax中的内存容量换为以byte为单位的
    mul cx

    shl edx, 16
    or edx, eax
    add edx, 0x100000          ; 加上低端1MB内存获得实际内存大小

.error_hlt:
    hlt

.mem_get_ok:
    mov [total_mem_bytes], edx ; 将内存换为byte为单位后存入total_mem_bytes处

    


; --- 准备进入保护模式 ---

; Step 1: 打开A20
in al, 0x92
or al, 0000_0010b
out 0x92, al

; Step 2: 加载GDT
lgdt [gdt_ptr]

; Step 3: CR0寄存器PE位，置为1
mov eax, cr0
or eax, 0x00000001
mov cr0, eax

; ----- 此时就算真正进入保护模式的大门了 -----

jmp dword SELECTOR_CODE:p_mode_start    ;刷新流水线

[bits 32]
p_mode_start:                           ;加载选择子到各段寄存器，从而更新段描述符缓冲寄存器令其从原先保存的实模式下的20位段                                        ;基址转变为保护模式下的整体(重新排列后)的段描述符
    
    mov ax, SELECTOR_DATA
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, LOADER_STACK_TOP
    mov ax, SELECTOR_VIDEO
    mov gs, ax

; -------------------  加载kernel进内存  --------------------------
mov eax, KERNEL_START_SECTOR                   ; kernel.bin所在的扇区号(0x9)
mov ebx, KERNEL_BIN_BASE_ADDR                 ; 从磁盘读出kernel.bin后存入内存中以ebx起始的地址(0x70000)
mov ecx, 200                                                  ;  需读入的扇区数

call rd_disk_m_32

; -------------------   创建页目录及页表   ------------------------
call setup_page      ; 调用setup_page函数
sgdt [gdt_ptr]         ; 读取GDTR寄存器的数据，存入gdt_ptr指向的内存单元上

mov ebx, [gdt_ptr + 2]                                   ; gdt_ptr+2内存上存储的是GDT基址, 该基址+0x18所指向的内存地址存储的是第3个段描述符，即显存段描述符
or dword [ebx + 0x18 + 4], 0xc0000000          ;  再+4指向的内存单元存储的就是显存段描述符高32位存储的内容，该32位中的最高位是段基址的第31~24位
                                                                     ; 直接跟0xc0000000或运算达到相加的目的，从而实现将显存段的段基址改为3GB以上的目的
add dword [gdt_ptr + 2], 0xc0000000             ; 将gdt的基址加上0xc0000000使其成为内核所在的高地址

add esp, 0xc0000000                                      ; 将栈指针同样映射到内核地址

mov eax, PAGE_DIR_TABLE_POS                     ; 把页目录地址赋给CR3寄存器
mov cr3, eax

mov eax, cr0                                                  ; 打开cr0的pg位(第31位)
or eax, 0x80000000
mov cr0, eax

lgdt [gdt_ptr]                                                  ; 开启分页后，用gdt新的地址重新加载(就是说gdt_ptr存储的内容已经变了，gdt基址已经变了，需要重新加载到GDTR)

; 其实当前已经是32位保护模式环境了，内核kernel.bin也是32位程序，按理说不需要“显式”重新清空流水线，但为了以防万一还是用无条件跳转指令刷新一下
jmp SELECTOR_CODE: enter_kernel                 ; 强制刷新流水线，更新gdt

enter_kernel:
    call kernel_init
    mov esp, 0xc009f000
    jmp KERNEL_ENTRY_POINT

; -------------------------- 将kernel.bin中的segment拷贝到各段被编译的虚拟地址处 --------------------------------
kernel_init:
    xor eax, eax                                               
    xor ebx, ebx                                               ; ebx记录程序头表地址
    xor ecx, ecx                                                ; ecx记录程序头表中的program header数量
    xor edx, edx                                               ; edx记录program header 的尺寸，即e_phentsize
    
    mov dx, [KERNEL_BIN_BASE_ADDR + 42]    ; 偏移42字节处的属性是e_phentsize，表示program header的大小
    mov ebx, [KERNEL_BIN_BASE_ADDR + 28]  ; 偏移文件开始部分28字节的地方是e_phoff, 即程序头表(第1个prgram header)在文件中的偏移量，在我自己编译的文件中是0x34
    
    add ebx, KERNEL_BIN_BASE_ADDR             ; 程序头表在文件中的偏移量 + 内核文件的加载地址 = 程序头表的基址，ebx此后用来遍历每一个段
    mov cx, [KERNEL_BIN_BASE_ADDR + 44]              ; 偏移文件44字节的地方是e_phnum, 表示有几个program header

;对每个段都需要做的动作
.each_segment:
        cmp byte [ebx + 0], PT_NULL                 ; 判断该段是否为空段类型
        je .PTNULL                                             ; 若是空段说明此program header未使用，则跨过该段不处理，调到.PTNULL处

        ; 为函数memcpy压入参数，参数是从右往左依次压入, 函数原型类似于memcpy(dst, src, size), 这里实现的是将段复制到自身被编译到的虚拟地址处
        push dword [ebx +16]                            ;压入函数的第三个参数size ( program header中偏移16字节的地方是p_filesz )
        
        mov eax, [ebx + 4]                                 ; 距程序头偏移量为4字节的位置是p_offset(本段在文件中的起始偏移地址)
        add eax, KERNEL_BIN_BASE_ADDR         ; 此时eax存储的是本段的物理地址
        
        push eax                                                ; 压入函数memcpy的第二个参数：源地址
        push dword [ebx + 8]                             ; 压入memcpy的第一个参数：目的地址 ( program header中偏移16字节的地方是p_vaddr)
        
        call mem_cpy                                         ;参数压栈完毕，调用memcpy完成段复制
        add esp, 12                                            ; 清理栈中压入的三个参数
       
.PTNULL:
    add ebx, edx                                              ; 跨段，令ebx指向下一个program header地址

    loop .each_segment
    ret       

; ----------------------------------  逐字节拷贝函数 mem_cpy(dst, src, size)  ------------------------------
mem_cpy:
    cld                                                             ; 清除方向标志，让数据的源地址和目的地址逐渐增大
    push ebp                                                   ; 将ebp原有的内容入栈备份, 方便下一条语句把esp赋给ebp，从而实现基于ebp访问栈中参数
    mov ebp, esp                                             ; 把esp的值赋给ebp备份保存, 因为下一行把ecx入栈了，esp的值会改变
    push ecx                                                    ; rep指令需要用到ecx进行逐字节拷贝，但是外层循环也要用来遍历每个段，因此先将ecx备份

    mov edi, [ebp + 8]                                      ; dst存到edi
    mov esi, [ebp + 12]                                     ; src存到esi
    mov ecx, [ebp + 16]                                    ; size存到ecx
    rep movsb                                                  ; 逐字节拷贝
    
    ; 恢复环境
    pop ecx                                                      ; 恢复ecx原来的值，继续遍历下一个段
    pop ebp                                                     ; 恢复ebp原有的值
    ret

setup_page:
    ; 先把页目录占用的内存空间逐字节清0
    mov ecx, 4096    ; 页目录表大小为4KB（4096 Bytes）
    mov esi, 0

.clear_page_dir:
    mov byte [PAGE_DIR_TABLE_POS + esi], 0    ; 从页目录表的基址开始逐字节清零
    inc esi                                   ; esi作为变址，自增1
    loop .clear_page_dir                      ; 循环执行，每执行一次cx的值就减1

; 开始创建页目录项(PDE)
.create_pde:
    mov eax, PAGE_DIR_TABLE_POS               ; 页目录表起始位置赋给eax
    add eax, 0x1000                           ; +4KB后，eax为页表基址，即第一个页表项的位置
    mov ebx, eax                              ; 再赋值给ebx，令ebx作为页表的基址，为.create_pte做准备
    
    ;
    or eax, PG_US_U | PG_RW_W | PG_P          ; 页目录项的属性RW和P位均为1，表示用户属性，所有特权级别都可以访问
    mov [PAGE_DIR_TABLE_POS + 0x0], eax       ; 这是第一个目录项的内容:写入第一个页表的位置(0x101000)及属性(7)
    
    mov [PAGE_DIR_TABLE_POS + 0xc00], eax     ; 0xc00/4=768表示第768个页表占用的目录项, 而0xc00以上的目录项表示的空间都用于内核空间, 第768个目录项和第1个目录项指向同一个页表地址

    sub eax, 0x1000                           ; 此时eax为0x100007
    mov [PAGE_DIR_TABLE_POS + 4092], eax      ; 使最后一个页目录项指向页目录表自己的地址虚拟内存最后一个目录项,为了动态操纵?

; ---- 下面开始创建页表项(PTE) ：这里就创建了"一个页表"的页表项 -----
    mov ecx, 256                              ; 低端1MB内存 / 每页大小4K = 256
    mov esi, 0
    mov edx, PG_US_U | PG_RW_W | PG_P         ; 属性为7, US=1, RW=1, P=1

.create_pte:
    mov [ebx+esi*4], edx                      ; 开始给页表项赋值，内容即是页表项对应的物理地址
    add edx, 4096                             ; edx存储的是物理地址和属性，因为一页大小为4KB，因此下一个物理页地址要+4096
    inc esi
    loop .create_pte                          ; 就只创建256个页表项，在低端1MB内存中，虚拟地址等于物理地址
    
; 创建内核其他页目录项（PDE）

;这里对于我们这里填写的目录项所对应的页表 页表中我们还没填写相应的值
;为了实现 真正意义上的 内核空间被用户进程完全共享只是把页目录与页表的映射做出来了
    mov eax, PAGE_DIR_TABLE_POS
    add eax, 0x2000                           ; 此时eax指向的是第二个页表的物理地址
    or eax, PG_US_U | PG_RW_W | PG_P          ; 页目录项的属性US, RW和P都为1
    
    mov ebx, PAGE_DIR_TABLE_POS
    mov ecx, 254                              ; 范围为第769~1022的所有目录项数量，第1023项我们已经映射到页目录项本身位置了
    mov esi, 769

.create_kernel_pde:
    mov [ebx+esi*4], eax
    inc esi
    add eax, 0x1000                           ; 赋给eax下一个页表(一个页表4KB)的物理地址
    loop .create_kernel_pde
    ret

;-------------------------------------------------------------------------------
;功能:读取硬盘n个扇区,只在mbr的rd_disk_m_16基础上把最后一部分的bx修改为ebx，因为现在的起始地址0x70000无法用16位寄存器来表示
rd_disk_m_32:	   
;-------------------------------------------------------------------------------
				       ; eax=LBA扇区号
				       ; ebx=将数据写入的内存地址
				       ; ecx=读入的扇区数
      mov esi,eax	  ;备份eax
      mov di,cx              ;备份cx
;读写硬盘:
; Step1：设置要读取的扇区数
      mov dx,0x1f2
      mov al,cl
      out dx,al            ;读取的扇区数

      mov eax,esi	   ;恢复ax

; Step2：将LBA地址存入0x1f3 ~ 0x1f6

      ;LBA地址7~0位写入端口0x1f3
      mov dx,0x1f3                       
      out dx,al                          

      ;LBA地址15~8位写入端口0x1f4
      mov cl,8
      shr eax,cl                   ;; eax为32位 ax为16位 eax的低位字节 右移8位即8~15
      mov dx,0x1f4
      out dx,al

      ;LBA地址23~16位写入端口0x1f5
      shr eax,cl
      mov dx,0x1f5
      out dx,al

    		 
    		  ; 24 25 26 27位 尽管我们知道ax只有2 但还是需要按规矩办事 
    		  ; 把除了最后四位的其他位置设置成0
    shr eax,cl
    
    and al,0x0f 
    or al,0xe0    ;!!! 把第7~4位设置成1110 转换为LBA模式
    mov dx,0x1F6  ; 参照硬盘控制器端口表 Device 
    out dx,al

; Step3: 向Command写操作 Status和Command一个寄存器
    mov dx,0x1f7 ; Status寄存器端口号
    mov al,0x20  ; 0x20是读命令
    out dx,al
    

; Step4: 向Status查看是否准备好，即检测硬盘状态
;设置不断读取重复 如果不为1则一直循环
  .not_ready:     
    nop           ; !!! 空跳转指令 在循环中达到延时目的
    in al,dx      ; 把寄存器中的信息返还出来
    and al,0x88   ;  0100 0100 0x88
    cmp al,0x08    ;第4位为1表示硬盘控制器已准备好数据传输，第7位为1表示硬盘忙
    jnz .not_ready ; !!! jump not equal == 0
    
; Step5: 从0x1f0端口读数据读取数据

    mov ax,di      ;把 di 储存的cx 取出来
    mov dx,256
    mul dx        ;与di 与 ax 做乘法 计算一共需要读多少次 方便作循环 低16位放ax 高16位放dx
    mov cx,ax      ;loop 与 cx相匹配 cx-- 当cx == 0即跳出循环
    mov dx,0x1F0
 .go_read_loop:
    in ax,dx      ;两字节dx, 一次读两字节
    mov [ebx],ax
    add ebx,2
    loop .go_read_loop
    
    ret ;与call 配对返回原来的位置 跳转到call下一条指令