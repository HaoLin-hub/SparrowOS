//
// Created by LinHao on 2023/4/25.
//
/****************** 注意机器模式的使用 ****************************
 * b -- 取寄存器中最低8位
 * w -- 取寄存器中2个字节的部分
 */
#ifndef __LIB_IO_H
#define __LIB_IO_H
#include "stdint.h"

/*向端口port写入一个字节*/
static inline void outb(uint16_t port, uint8_t data){
    // N为立即数约束,表示0~255之间的立即数
    // d为寄存器约束, 表示edx/dx/dl 存储端口号
    // %b0表示对应al, %w1表示对应dx
    asm volatile ("outb %b0, %w1" : : "a"(data), "Nd"(port));
}

/*将addr处起始的word_cnt个字写入端口port*/
static inline void outsw(uint16_t port, const void* addr, uint32_t word_cnt){
    // +为操作数类型修饰符, 表示操作数是既可读又可写的,！！！告诉gcc：所约束的寄存器或内存先被读入, 后被写入
    // outsw是把ds:esi处的16位内容写入端口port中, 我们在设置段描述符的时候已经将ds, es, ss段的选择子都设置为相同的值了，此时不用担心数据错乱
    asm volatile ("cld; rep outsw" : "+S"(addr), "+c"(word_cnt) : "d"(port));
}

/*将从端口port读入的一个字节返回*/
static inline uint8_t inb(uint16_t port){
    uint8_t data;
    asm volatile ("inb %w1, %b0" : "=a"(data) : "Nd"(port));
    return data;
}

/*将从端口port读入的word_cnt个字写入addr*/
static inline void insw(uint16_t port, void* addr, uint32_t word_cnt){
    // insw是将端口port处读入的16位内容写入到es:edi指向的内存
    asm volatile ("cld; rep insw" : "+D"(addr), "+c"(word_cnt) : "d"(port) : "memory");
}


#endif
