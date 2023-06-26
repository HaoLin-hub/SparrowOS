;主引导程序 
;------------------------------------------------------------
%include "boot.inc"
SECTION MBR vstart=0x7c00 ;起始地址编译在0x7c00
    mov ax,cs
    mov ds,ax
    mov es,ax
    mov ss,ax
    mov fs,ax
    mov sp,0x7c00
    mov ax,0xb800  ; ax为文本信号起始区
    mov gs,ax      ; gs = ax 充当段基址的作用
    
    		   ; ah = 0x06 al = 0x00 想要调用int 0x06的BIOS提供的中断对应的函数 
    		   ; 即向上移动即完成清屏功能
    		   ; cx dx 分别存储左上角与右下角的左边 详情看int 0x06函数调用
    mov ax,0600h 
    mov bx,0700h
    mov cx,0
    mov dx,184fh
    
    ;调用BIOS中断
    int 0x10 

   ; 输出字符串:MBR
   mov byte [gs:0x00],'1'
   mov byte [gs:0x01],0xA4

   mov byte [gs:0x02],' '
   mov byte [gs:0x03],0xA4

   mov byte [gs:0x04],'M'
   mov byte [gs:0x05],0xA4	   ;A表示绿色背景闪烁，4表示前景色为红色

   mov byte [gs:0x06],'B'
   mov byte [gs:0x07],0xA4

   mov byte [gs:0x08],'R'
   mov byte [gs:0x09],0xA4
	 
   mov eax,LOADER_START_SECTOR	 ; 起始扇区lba地址
   mov bx,LOADER_BASE_ADDR       ; 写入的地址
   mov cx,4			 ; 待读入的扇区数
   call rd_disk_m_16		 ; 以下读取程序的起始部分（一个扇区）
  
   jmp LOADER_BASE_ADDR + 0x300
       
;-------------------------------------------------------------------------------
;功能:读取硬盘n个扇区
rd_disk_m_16:	   
;-------------------------------------------------------------------------------
				       ; eax=LBA扇区号
				       ; ebx=将数据写入的内存地址
				       ; ecx=读入的扇区数
      mov esi,eax	  ;备份eax
      mov di,cx		  ;备份cx
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
    in ax,dx      ;两字节dx 一次读两字
    mov [bx],ax
    add bx,2
    loop .go_read_loop
    
    ret ;与call 配对返回原来的位置 跳转到call下一条指令
        
    times 510 - ($ - $$) db 0 
    db 0x55,0xaa

