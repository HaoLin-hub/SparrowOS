#include "sync.h"
#include "list.h"
#include "global.h"
#include "debug.h"
#include "interrupt.h"

void sema_init(struct semaphore* sema, uint8_t value){
    sema->value = value;        // 为信号量赋初值
    list_init(&sema->waiters);  // 初始化信号量的等待队列
}

/* 初始化锁plock */
void lock_init(struct lock* plock){
    plock->holder = NULL;
    plock->holder_repeat_nr = 0;
	sema_init(&plock->semaphore, 1);    // 用二元信号量也实现锁
}

/* 信号量down操作: P操作 */
void sema_down(struct semaphore* psem){
    // 关中断来保证原子操作
    enum intr_status old_status =  intr_disable();
    while (psem->value == 0){    // 表示资源已被别的线程占用
        ASSERT(!elem_find(&psem->waiters, &running_thread()->general_tag)); // 断言当前线程(状态为TASK_RUNNING)不在信号量的waiters队列中
        if(elem_find(&psem->waiters, &running_thread()->general_tag)){
            PANIC("sem_down: thread blocked has been in waiters_list\n");
        }
        // 若信号量的值等于0，则当前线程把自己加入该锁的等待队列，然后阻塞自己, 直到被唤醒
        list_append(&psem->waiters, &running_thread()->general_tag);
        thread_block(TASK_BLOCKED);
    }
    // 若psem信号量的值为1, 则说明此时尚无线程占用该锁，会执行下面的代码,也就是获得锁
    psem->value--;
    ASSERT(psem->value == 0);
    // 恢复之前的中断状态
    intr_set_status(old_status);
}

/* 信号量的up操作：V操作 */
void sema_up(struct semaphore* psem){
    // 关中断保证原子操作
    enum intr_status old_status = intr_disable();
    ASSERT(psem->value == 0);
    // 若等待队列不为空
    if(!list_empty(&psem->waiters)) {
        // 获得等待队列中的队首线程的PCB
        struct task_struct* thread_blocked = elem2entry(struct task_struct, general_tag, list_pop(&psem->waiters));
        thread_unblock(thread_blocked);
    }
    psem->value++;
    ASSERT(psem->value == 1);
    // 恢复之前的中断状态
    intr_set_status(old_status);
}

/* 获取锁plock */
void lock_acquire(struct lock* plock){
    // 排除自己已经持有锁但未将其释放的情况
    if(plock->holder != running_thread()){
        // down操作申请锁, 为原子操作
        sema_down(&plock->semaphore);
        plock->holder = running_thread();
        ASSERT(plock->holder_repeat_nr == 0);
        plock->holder_repeat_nr = 1;    // 表示当前线程第一次申请了这个锁
    }else{
        plock->holder_repeat_nr++;
    }
}

/* 释放锁plock */
void lock_release(struct lock* plock){
    // 绝不会有 自己没有锁却意图释放的情况
    ASSERT(plock->holder == running_thread());
    // 如果持有者多次申请了该锁, 则调用lock_release函数此时还不能真正将锁释放
    if(plock->holder_repeat_nr > 1) {
        plock->holder_repeat_nr--;
        return;
    }
    ASSERT(plock->holder_repeat_nr == 1);

    plock->holder = NULL;         // 锁的持有者置为空（！！！必须在sema_up之前！！！）
    plock->holder_repeat_nr = 0;
    sema_up(&plock->semaphore);    // 信号量的up操作(V操作)
}