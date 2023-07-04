[bits 32]
extern  main
extern	 exit 
section  .text
global  _start
_start:
    ; 下面这两个要和execv中调用load函数之后指定的寄存器一致
    push ebx      ; 将argv的地址压入栈中
    push ecx       ; 将参数个数argc压入栈中
    call main       ; 调用main函数

    ; 将main函数的返回值通过栈传递给exit, gcc用eax存储返回值, 这是ABI规定的
    push eax
    call exit
    