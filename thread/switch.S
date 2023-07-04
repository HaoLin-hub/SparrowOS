[bits 32]
section .text
global switch_to
switch_to:
    ; 栈中此处是返回地址
    push esi
    push edi
    push ebx
    push ebp

    mov eax, [esp + 20]    ; 得到栈中的参数cur, cur = [esp + 20]
    mov [eax], esp            ;  将栈顶指针esp保存到当前线程cur_thread的self_kstack字段中(因为pcb结构体的开头4字节就是self_kstack字段)

;--------------- 以上是备份当前线程cur_thread的环境，下面是恢复下一个线程的环境 -------------

mov eax, [esp +24]    ; 得到栈中的参数next, 此时eax的值可以认为是next线程PCB中的self_kstack的地址
mov esp, [eax]           ; 将next线程的栈顶指针恢复到esp中
pop ebp
pop ebx
pop edi
pop esi
ret                             ; 此时栈顶指针esp指向返回地址，ret将该返回地址加载到处理器的eip寄存器中，从而使next线程的代码恢复执行

