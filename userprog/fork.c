#include "fork.h"
#include "process.h"
#include "memory.h"
#include "interrupt.h"
#include "debug.h"
#include "thread.h"
#include "string.h"
#include "file.h"
#include "pipe.h"

extern void intr_exit(void);

/* 将父进程的pcb拷贝给子进程, 成功返回0, 失败返回-1 */
static int32_t copy_pcb_vaddrbitmap_stack0(struct task_struct* child_thread, struct task_struct* parent_thread) {
    // 直接复制父进程pcb所在的整个页, 里面包含了进程pcb信息以及特权0级的栈(里面包含了返回地址)。
    memcpy(child_thread, parent_thread, PG_SIZE);
    // 单独修改pcb各个项
    child_thread->pid = fork_pid();
    child_thread->elapsed_ticks = 0;
    child_thread->status = TASK_READY;
    child_thread->ticks = child_thread->priority;  // 为新进程把时间片充满
    child_thread->parent_pid = parent_thread->pid;
    child_thread->general_tag.prev = child_thread->general_tag.next = NULL;  // 确保新进程的pcb不在就绪队列上
    child_thread->all_list_tag.prev = child_thread->all_list_tag.next = NULL; // 确保新进程的pcb也不在全局队列上
    block_desc_init(child_thread->u_block_desc);  // 初始化新进程自己的内存块描述符, 如果没初始化将继承父进程的块描述符，新进程进行内存分配时会出现缺页异常

    // 子进程不能和父进程共用"同一个用户进程虚拟地址池", 需要将父进程的虚拟地址池原模原样的复制给子进程
    uint32_t bitmap_pg_cnt = DIV_ROUND_UP((0xc0000000 - USER_VADDR_START) / PG_SIZE / 8, PG_SIZE);
    void* vaddr_btmp = get_kernel_pages(bitmap_pg_cnt);
    if(vaddr_btmp == NULL) {
        return -1;
    }
    memcpy(vaddr_btmp, child_thread->userprog_vaddr.vaddr_bitmap.bits, bitmap_pg_cnt * PG_SIZE);
    child_thread->userprog_vaddr.vaddr_bitmap.bits = vaddr_btmp;  // 完成复制

    /** 调试用 **/
    ASSERT(strlen(child_thread->name) < 11);	// pcb.name的长度是16,为避免下面strcat越界
    strcat(child_thread->name,"_fork");

    return 0;
}

/* 复制父进程的进程体(代码和数据)以及用户栈 到子进程 */
static void copy_body_stack3(struct task_struct* child_thread, struct task_struct* parent_thread, void* buf_page) {
    // 准备好各变量, 指向父进程中相关资源, 为后续复制做准备
    uint8_t* vaddr_btmp = parent_thread->userprog_vaddr.vaddr_bitmap.bits;
    uint32_t btmp_bytes_len = parent_thread->userprog_vaddr.vaddr_bitmap.btmp_bytes_len;
    uint32_t vaddr_start = parent_thread->userprog_vaddr.vaddr_start;
    uint32_t idx_byte = 0;    // 位图中"以字节为单位"的位置
    uint32_t idx_bit = 0;     // 位图中"以bit为单位"的位置
    uint32_t prog_vaddr = 0;

    // 在父进程的用户空间(用户使用的内存是用pcb中的userprog_vaddr虚拟地址池来管理的)中查找已有数据的页
    while (idx_byte < btmp_bytes_len) {
        if (vaddr_btmp[idx_byte] != 0){    // 若父进程的用户空间位图的第"idx_byte"个字节的内容不为0,则说明该字节内有被分配物理内存的虚拟地址
            idx_bit = 0;
            while (idx_bit < 8) {  // 遍历字节内的每个bit
                if ((BITMAP_MASK << idx_bit) & vaddr_btmp[idx_byte]) {
                    prog_vaddr = (idx_byte * 8 + idx_bit) * PG_SIZE + vaddr_start;  // 计算出bit为1的位对应的用户空间虚拟地址

                    /** 下面的操作是将父进程用户空间中的数据通过内核空间做中转,最终复制到子进程的用户空间 **/
                    memcpy(buf_page, (void*)prog_vaddr, PG_SIZE);  // 将父进程在用户空间中的数据复制到内核缓冲区buf_page,目的是下面切换到子进程的页表后,还能访问到父进程的数据

                    page_dir_activate(child_thread);  // 将页表切换到子进程, 目的是避免下面申请内存的函数将pte以及pde安装在父进程的页表中
                    get_a_page_without_opvaddrbitmap(PF_USER, prog_vaddr);    // 在子进程中申请虚拟地址prog_vaddr(因为虚拟地址位图已经在上个函数中复制过了)

                    memcpy((void*)prog_vaddr, buf_page, PG_SIZE);  // 从内核缓冲区中将父进程数据复制到子进程的用户空间

                    page_dir_activate(parent_thread);    // 恢复父进程页表
                }
                idx_bit++;
            }
        }
        idx_byte++;
    }
}

/* 为子进程构建thread_stack和修改返回值 */
static int32_t build_child_stack(struct task_struct* child_thread) {
    // 获得子进程中断栈的栈顶
    struct intr_stack* intr_0_stack = (struct intr_stack*)((uint32_t)child_thread + PG_SIZE - sizeof(struct intr_stack));
    // 修改子进程的返回值为0(根据abi约定, eax寄存器中的是系统调用函数的返回值)
    intr_0_stack->eax = 0;

    /** 为switch_to 构建 struct thread_stack, 将其构建在紧邻intr_stack之下的空间 **/
    uint32_t* ret_addr_in_thread_stack = (uint32_t*)intr_0_stack - 1;  // thread_stack中eip的位置

    uint32_t* esi_ptr_in_thread_stack = (uint32_t*)intr_0_stack - 2;
    uint32_t* edi_ptr_in_thread_stack = (uint32_t*)intr_0_stack - 3;
    uint32_t* ebx_ptr_in_thread_stack = (uint32_t*)intr_0_stack - 4;

    uint32_t* ebp_ptr_in_thread_stack = (uint32_t*)intr_0_stack - 5;   // ebp在thread_stack中的地址便是当时的esp(thread_stack / 0级栈的栈顶)
    // 把构建的thread_stack的栈顶作为Switch_to恢复数据时的栈顶
    child_thread->self_kstack = ebp_ptr_in_thread_stack;

    // switch_to函数的返回地址更新为intr_exit, 直接从中断返回
    *ret_addr_in_thread_stack = (uint32_t)intr_exit;

    return 0;  // 运行完这个函数后，其实子进程就已经创建好了
}

/* fork之后, 更新线程thread的inode打开数 */
static void update_inode_open_cnts(struct task_struct* thread) {
    // 遍历fd_table中除前3个标准文件描述符之外的所有文件描述符
    int32_t local_fd = 3, global_fd = 0;
    while (local_fd < MAX_FILES_OPEN_PER_PROC){
        global_fd = thread->fd_table[local_fd];
        ASSERT(global_fd < MAX_FILE_OPEN);
        if(global_fd != -1) {  // 将进程中打开过的所有文件, 其对应的打开数都 + 1
            if(is_pipe(local_fd)) {  // 如果打开的是管道, 子进程跟父进程是共享的
                file_table[global_fd].fd_pos++;
            } else {
                file_table[global_fd].fd_inode->i_open_cnts++;
            }
        }
        local_fd++;
    }
}

/* 拷贝父进程本身所占资源给子进程, 成功返回0, 失败返回-1 (此函数是上面函数的封装) */
static int32_t copy_process(struct task_struct* child_thread, struct task_struct* parent_thread) {
    // 申请一页内核缓冲区, 作为父进程用户空间的数据复制到子进程用户空间的中转
    void* buf_page = get_kernel_pages(1);
    if(buf_page == NULL) {
        return -1;
    }

    // 1. 复制父进程的pcb, 虚拟地址位图, 内核栈 到子进程
    if (copy_pcb_vaddrbitmap_stack0(child_thread, parent_thread) == -1){
        return -1;
    }

    // 2. 为子进程创建页表, 此页表仅包括内核空间(内核页表)
    child_thread->pgdir = create_page_dir();
    if(child_thread->pgdir == NULL) {
        return -1;
    }

    // 3. 复制父进程的进程体(代码和数据)以及用户栈给子进程
    copy_body_stack3(child_thread, parent_thread, buf_page);

    // 4. 构建子进程的Thread_stack, 并修改系统调用返回值
    build_child_stack(child_thread);

    // 5. 更新文件inode的打开数
    update_inode_open_cnts(child_thread);

    mfree_page(PF_KERNEL, buf_page, 1);
    return 0;
}

/* fork的内核实现部分，fork子进程, 内核线程不可调用 */
pid_t sys_fork(void) {
    struct task_struct* parent_thread = running_thread();
    // 先获得一页内核空间作为子进程的pcb
    struct task_struct* child_thread = get_kernel_pages(1);
    if (child_thread == NULL) {
        return -1;
    }
	ASSERT(INTR_OFF == intr_get_status() && parent_thread->pgdir != NULL);
    // 执行克隆进程操作
    if (copy_process(child_thread, parent_thread) == -1){
        return -1;
    }
    // 将子进程加入到就绪队列和全局队列
    ASSERT(!elem_find(&thread_ready_list, &child_thread->general_tag));
    list_append(&thread_ready_list, &child_thread->general_tag);
    ASSERT(!elem_find(&thread_all_list, &child_thread->all_list_tag));
    list_append(&thread_all_list, &child_thread->all_list_tag);

    return child_thread->pid;  // 对于父进程来说, 返回的是子进程的pid
}