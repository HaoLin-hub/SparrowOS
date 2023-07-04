#include "wait_exit.h"
#include "global.h"
#include "debug.h"
#include "thread.h"
#include "list.h"
#include "stdio-kernel.h"
#include "memory.h"
#include "bitmap.h"
#include "fs.h"
#include "file.h"
#include "pipe.h"

/* 回收用户进程的资源：1. 页表中对应的物理页 2. 虚拟内存池所占物理页框 3. 关闭打开的文件 */
static void release_prog_resource(struct task_struct* release_thread) {
    uint32_t* pgdir_vaddr = release_thread->pgdir;
    uint16_t user_pde_nr = 768, pde_idx = 0;         // 分别表示用户空间中的pde数量, 以及pde的初始索引值
    uint32_t pde = 0;                                // 用来接收每个页目录项的“内容”
    uint32_t* v_pde_ptr = NULL;

    uint16_t user_pte_nr = 1024, pte_idx = 0;        // 分别表示用户每个页表中pte的数量, 以及pte的初始索引值
    uint32_t pte = 0;                                // 用来接收每个页表项的“内容”
    uint32_t* v_pte_ptr = NULL;

    uint32_t* first_pte_vaddr_in_pde = NULL;         // 表示pde中第0个pte的地址, 用它来遍历页表中的所有pte
    uint32_t pg_phy_addr = 0;

    /*** (1) 开始回收页表中用户空间的页框  ***/
    while (pde_idx < user_pde_nr){
        v_pde_ptr = pgdir_vaddr + pde_idx;
        pde = *v_pde_ptr;
        if (pde & 0x00000001) {  // 如果页目录项的p位为1, 说明该页目录项表示的页表中“可能”存在页表项
            first_pte_vaddr_in_pde = pte_ptr(pde_idx * 0x400000);  // 一个页表所能表示的内存容量为0x400000字节
            pte_idx = 0;
            while (pte_idx < user_pte_nr) {
                v_pte_ptr = first_pte_vaddr_in_pde + pte_idx;
                pte = *v_pte_ptr;
                if(pte & 0x00000001) {    // 如果页表项的P位为1, 表明确实分配了物理地址给该虚拟页
                    // 将pte中记录的物理页框直接在相应的物理内存池位图中清0
                    pg_phy_addr = pte & 0xfffff000;
                    free_a_phy_page(pg_phy_addr);
                }
                pte_idx++;
            }
            // 将pde中记录的物理页框直接在相应物理内存池的位图中清0
            pg_phy_addr = pde & 0xfffff000;
            free_a_phy_page(pg_phy_addr);
        }
        pde_idx++;
    }

    /*** (2) 回收用户虚拟地址池所占的物理内存 ***/
    uint32_t bitmap_pg_cnt = (release_thread->userprog_vaddr.vaddr_bitmap.btmp_bytes_len) / PG_SIZE;   // 用户虚拟地址池所占的页数量
    uint8_t* user_vaddr_pool_bitmap = release_thread->userprog_vaddr.vaddr_bitmap.bits;                // 获得用户虚拟地址池的起始虚拟地址
    mfree_page(PF_KERNEL, user_vaddr_pool_bitmap, bitmap_pg_cnt);                                      // 从内核物理内存池中删除用户虚拟地址池

    /*** （3） 关闭用户进程打开的文件  ***/
    uint8_t local_fd = 3;
    while(local_fd < MAX_FILES_OPEN_PER_PROC) {
        if (release_thread->fd_table[local_fd] != -1) {
            if (is_pipe(local_fd)){
                uint32_t global_fd = fd_local2global(local_fd);
                if(--file_table[global_fd].fd_pos == 0){
                    mfree_page(PF_KERNEL, file_table[global_fd].fd_inode, 1);
                    file_table[global_fd].fd_inode = NULL;
                }
            } else {
                sys_close(local_fd);
            }
        }
        local_fd++;
    }
}

/* 查找父进程pid为ppid的子进程, 成功返回true, 失败返回false, 供list_traversal调用的回调函数 */
static bool find_child(struct list_elem* pelem, int32_t ppid) {
    struct task_struct* pthread = elem2entry(struct task_struct, all_list_tag, pelem);
    if (pthread->parent_pid == ppid) {
        return true;
    }
    return false;   // 让list_traversal继续传递下一个元素
}

/* 查找状态为TASK_HANGING, 且父进程pid为ppid的子进程, 供list_traversal的回调函数 */
static bool find_hanging_child(struct list_elem* pelem, int32_t ppid) {
    struct task_struct* pthread = elem2entry(struct task_struct, all_list_tag, pelem);
    if (pthread->parent_pid == ppid && pthread->status == TASK_HANGING) {
        return true;
    }
    return false;
}

/* 将parent_id 等于 pid的子进程过继给init, list_traversal的回调函数 */
static bool init_adopt_a_child(struct list_elem* pelem, int32_t pid) {
    struct task_struct* pthread = elem2entry(struct task_struct, all_list_tag, pelem);
    if (pthread->parent_pid == pid) {     // 若该进程的parent_pid为pid,返回
        pthread->parent_pid = 1;
    }
    return false;		// 让list_traversal继续传递下一个元素
}

/* 等待子进程调用exit, 将子进程的退出状态保存到status指向的变量, 成功则返回子进程的pid, 失败则返回-1 */
pid_t sys_wait(int32_t* status) {
    struct task_struct* parent_thread = running_thread();
    while (1) {
        // 先从thread_all_list中找到当前进程的所有挂起状态的子进程, 优先处理已经是挂起状态的任务
        struct list_elem* child_elem = list_traversal(&thread_all_list, find_hanging_child, parent_thread->pid);
        // 如果确实找到了退出的子进程, 开始善后工作
        if(child_elem != NULL) {
            struct task_struct* child_thread = elem2entry(struct task_struct, all_list_tag, child_elem);
            *status = child_thread->exit_status;    // 从子进程的exit_status中获取子进程的状态存入*status
            // thread_exit之前,提前获取退出的子进程的pid
            uint16_t child_pid = child_thread->pid;
            // 从就绪队列和全部队列中删除进程表项, 传入第二个参数为false是为了使thread_exit后回到此处继续运行
            thread_exit(child_thread, false);

            return child_pid;
        }
        // 判断是否有还在运行的子进程
        child_elem = list_traversal(&thread_all_list, find_child, parent_thread->pid);
        if(child_elem == NULL) {
            return -1;
        } else {   // 确实有还在运行的子进程, 则将自己挂起, 直到子进程执行exit时将自己唤醒
            thread_block(TASK_WAITING);
        }
    }
}

/* 子进程用来结束自己时调用 */
void sys_exit(int32_t status) {
    struct task_struct* child_thread = running_thread();
    child_thread->exit_status = status;
    if (child_thread->parent_pid == -1) {
        PANIC("sys_exit: child_thread->parent_pid is -1\n");
    }

    /* 将进程child_thread的所有子进程都过继给init */
    list_traversal(&thread_all_list, init_adopt_a_child, child_thread->pid);

    /* 回收进程child_thread的资源 */
    release_prog_resource(child_thread);

    /* 如果父进程正在等待子进程退出,将父进程唤醒 */
    struct task_struct* parent_thread = pid2thread(child_thread->parent_pid);
    if (parent_thread->status == TASK_WAITING) {
        thread_unblock(parent_thread);
    }

    /* 将自己挂起,等待父进程获取其status,并回收其pcb */
    thread_block(TASK_HANGING);
}
