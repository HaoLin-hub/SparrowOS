#include "ioqueue.h"
#include "interrupt.h"
#include "global.h"
#include "debug.h"

/* 初始化io队列(输入缓存队列) */
void ioqueue_init(struct ioqueue* ioq){
    lock_init(&ioq->lock);    // 初始化io队列的锁
    ioq->producer = NULL;
    ioq->consumer = NULL;
    ioq->head = ioq->tail = 0;    // 头尾一开始均指向队头位置(数组0索引处)
}

/* 返回指定位置pos在缓冲区中的下一个位置 */
static int32_t next_pos(int32_t pos) {
    return (pos + 1) % bufsize;
}

/* 判断队列是否已满 */
bool ioq_full(struct ioqueue* ioq){
    ASSERT(intr_get_status() == INTR_OFF);
    return next_pos(ioq->head) == ioq->tail;
}

/* 判断队列是否已空 */
static bool ioq_empty(struct ioqueue* ioq) {
    ASSERT(intr_get_status() == INTR_OFF);
    return ioq->tail == ioq->head;
}

/* 使当前生产者或消费者在此缓冲区上等待 */
static void ioq_wait(struct task_struct** waiter) {
    ASSERT(*waiter == NULL && waiter != NULL);
    // 将当前线程 记录在waiter指向的指针中
    *waiter = running_thread();
    thread_block(TASK_BLOCKED);
}

/* 唤醒waiter */
static void wakeup(struct task_struct** waiter){
    ASSERT(*waiter != NULL);
    thread_unblock(*waiter);
    *waiter = NULL;
}

/* 消费者线程从ioq队列中获取一个字符 */
char ioq_getchar(struct ioqueue* ioq) {
    ASSERT(intr_get_status() == INTR_OFF);
    // 若缓冲区(队列)为空, 说明无法读取字符, 那把消费者ioq->consumer记为当前线程自己,然后阻塞自己,
    // 另一个目的是将来生产者往缓冲区里装商品后, 生产者知道唤醒哪个消费者, 也就是唤醒自己
    while (ioq_empty(ioq)) {
        lock_acquire(&ioq->lock);
        ioq_wait(&ioq->consumer);    // 拿着锁在这里阻塞
        lock_release(&ioq->lock);
    }

    char byte = ioq->buf[ioq->tail];   // 从缓冲区中取出
    ioq->tail = next_pos(ioq->tail);   // 更新缓冲区中读取字符的位置

    if(ioq->producer != NULL) {
        wakeup(&ioq->producer);    // 唤醒生产者
    }

    return byte;
}

/* 生产者线程往ioq队列中写入一个字符 */
void ioq_putchar(struct ioqueue* ioq, char byte) {
    ASSERT(intr_get_status() == INTR_OFF);
    // 若缓冲区(队列)已满, 则无法再写入字符, 那把生产者ioq->producer记为当前线程自己, 然后阻塞自己,
    // 另一个目的是将来消费者从缓冲区里读出数据后, 消费者知道要唤醒哪个生产者, 也就是唤醒自己本身
    while (ioq_full(ioq)) {
        lock_acquire(&ioq->lock);
        ioq_wait(&ioq->producer);    // 拿着锁在这里阻塞
        lock_release(&ioq->lock);
    }
    ioq->buf[ioq->head] = byte;    // 把字节放入缓冲区中
	ioq->head = next_pos(ioq->head);   // 更新缓冲区中写入字符的位置

    // 如果缓冲区中有正在阻塞的消费者
    if(ioq->consumer != NULL){
        wakeup(&ioq->consumer);
    }
}

/* 返回环形缓冲区中的数据长度 */
uint32_t ioq_length(struct ioqueue* ioq) {
    uint32_t len = 0;
    if(ioq->head >= ioq->tail) {
        len = ioq->head - ioq->tail;
    } else {
        len = bufsize - (ioq->tail - ioq->head);
    }
    return len;
}