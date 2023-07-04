#ifndef __DEVICE_IOQUEUE_H
#define __DEVICE_IOQUEUE_H
#include "stdint.h"
#include "thread.h"
#include "sync.h"

#define bufsize 2048    // 兼容管道的一页大小的环形缓冲队列

/* 环形队列：生产者消费者问题 */
struct ioqueue {
    // 本环形输入缓冲区的锁, 用于保证对该缓冲区的互斥操作
    struct lock lock;
    // 生产者线程, 缓冲区不满时就继续往里面放数据, 否则就睡眠, 此记录当缓冲区满时在此缓冲区睡眠的生产者线程
    struct task_struct* producer;
    // 消费者线程, 缓冲区不空时就继续从里面拿数据, 否则就睡眠, 此记录当缓冲区为空时在此缓冲区睡眠的消费者线程
    struct task_struct* consumer;
    char buf[bufsize];    // 缓冲区大小
    int32_t head;         // 环形队列队首地址
    int32_t tail;         // 环形队列队尾地址
};

void ioqueue_init(struct ioqueue* ioq);
bool ioq_full(struct ioqueue* ioq);
char ioq_getchar(struct ioqueue* ioq);
void ioq_putchar(struct ioqueue* ioq, char byte);
uint32_t ioq_length(struct ioqueue* ioq);
#endif