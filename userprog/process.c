#include "process.h"
#include "global.h"
#include "debug.h"
#include "memory.h"
#include "thread.h"
#include "list.h"
#include "tss.h"
#include "interrupt.h"
#include "string.h"
#include "console.h"

extern void intr_exit(void);

/* 构建用户进程初始化上下文信息 */
void start_process(void* filename) {
    void* function = filename;
    struct task_struct* cur = running_thread();
    // 为引用中断栈, 跨过线程栈将self_kstack指向中断栈栈顶
    cur->self_kstack += sizeof(struct thread_stack);
    struct intr_stack* proc_stack = (struct intr_stack*)cur->self_kstack;           // 初始化8个通用寄存器
    proc_stack->edi = proc_stack->esi = proc_stack->ebp = proc_stack->esp_dummy = 0;
    proc_stack->ebx = proc_stack->edx = proc_stack->ecx = proc_stack->eax = 0;
    // 继续初始化其他寄存器
    proc_stack->gs = 0;    // 用户态用不上, 直接初始化为0
    proc_stack->ds = proc_stack->es = proc_stack->fs = SELECTOR_U_DATA;
    proc_stack->eip = function;    // 待执行的“用户程序”地址
                                   // 程序能上CPU运行, 原因就是CS:[E]IP指向了程序入口地址, 这里先用参数filename对栈中的eip赋值(假装中断返回)

    proc_stack->cs = SELECTOR_U_CODE; // 将栈中代码段寄存器cs赋值为先前我们已在GDT中安装好的用户级代码段
    //接下来对栈中的eflags赋值
    proc_stack->eflags = (EFLAGS_IOPL_0 | EFLAGS_MBS | EFLAGS_IF_1);
    // 为用户进程分配3特权级下的栈, 也就是需要指向从用户内存池中分配的地址
    proc_stack->esp = (void*)((uint32_t)get_a_page(PF_USER, USER_STACK3_VADDR) + PG_SIZE);
    proc_stack->ss = SELECTOR_U_DATA;
    // 将当前栈顶esp替换为刚刚填充完的proc_stack, 然后通过jmp intr_exit使程序流程跳转到中断出口地址Intr_exit, 通过那里的一系列pop指令和iretd指令
    // 将proc_stack中的数据载入CPU各寄存器中, 从而使程序“假装”退出中断, 进入特权级3
    asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g"(proc_stack) : "memory");
}

/* 激活页表 */
void page_dir_activate(struct task_struct* p_thread) {
    /**执行此函数时, p_thread可能是内核线程。内核线程与内核共享同一套页表，而上一次被调度的可能是进程，如果不恢复线程所属的内核页表的话，该内核线程就会使用上次进程的页表**/
    uint32_t pagedir_phy_addr = 0x100000;    // 若为内核线程，需要重新填充页表的物理地址为0x100000

    // (默认为内核的页目录物理地址)
    if (p_thread->pgdir != NULL) {    // 用户态进程有自己的页目录表
        pagedir_phy_addr = addr_v2p((uint32_t)p_thread->pgdir);
    }
    // 更新页目录寄存器cr3, 使得新页表生效
    asm volatile ("movl %0, %%cr3" : : "r"(pagedir_phy_addr) : "memory");
}

/* 激活线程或进程的页表, 更新tss中的esp0为进程的特权级0的栈 */
void process_activate(struct task_struct* p_thread) {
    ASSERT(p_thread != NULL);
    // 激活线程或进程的页表
    page_dir_activate(p_thread);

    // 内核线程特权级本身就是0，处理器进入中断时并不会从tss中获取0特权级栈地址, 故不需要更新esp0
    if(p_thread->pgdir) {
        // 更新该进程的esp0, 用于此进程被中断时保留上下文
        update_tss_esp(p_thread);
    }
}

/* 创建页目录表, 将当前页表的表示内核空间的pde复制, 成功则返回页目录表的虚拟地址, 否则返回-1 */
uint32_t* create_page_dir(void) {
    // 用户进程的页表不能让用户直接访问到,所以在内核空间来申请
    uint32_t* page_dir_vaddr = get_kernel_pages(1);
    if(page_dir_vaddr == NULL){
        console_put_str("create_page_dir: get_kernel_page failed!");
        return NULL;
    }

    /*********** 1. 先复制页目录表 *************/
    // page_dir_vaddr是为用户进程申请的页目录表基址, 0x300是十进制的768, 4是每个页目录项的大小, 故0x300*4表示第768个页目录项的偏移量
    memcpy((uint32_t*) ((uint32_t)page_dir_vaddr + 0x300*4), (uint32_t*)(0xfffff000 + 0x300*4), 1024);

    /*********** 2. 再更新页目录地址，把最后一个页目录项更新为用户进程自己的页目录表 **********/
    uint32_t new_page_dir_phy_addr = addr_v2p((uint32_t)page_dir_vaddr);    // 将页目录表基址转换为物理地址
    page_dir_vaddr[1023] = new_page_dir_phy_addr | PG_US_U | PG_RW_W | PG_P_1;

    return page_dir_vaddr;
}

/* 创建用户虚拟地址位图 */
void create_user_vaddr_bitmap(struct task_struct* user_prog) {
    user_prog->userprog_vaddr.vaddr_start = USER_VADDR_START;    // 0x8048000
    uint32_t bitmap_pg_cnt = DIV_ROUND_UP((0xc0000000 - USER_VADDR_START) / PG_SIZE / 8, PG_SIZE);    // 记录位图需要的内存页框数(1 bit 表示 一页, 故/8表示的是位图占用多少字节)
    user_prog->userprog_vaddr.vaddr_bitmap.bits = get_kernel_pages(bitmap_pg_cnt);    // 为位图分配内存
    user_prog->userprog_vaddr.vaddr_bitmap.btmp_bytes_len = (0xc0000000 - USER_VADDR_START) / PG_SIZE / 8;
    bitmap_init(&user_prog->userprog_vaddr.vaddr_bitmap);   // 位图初始化
}

/* 创建用户进程 */
void process_execute(void* filename, char* name) {
    // pcb内核数据结构, 由内核来维护进程信息，因此需要在内核内存池中申请
    struct task_struct* thread = get_kernel_pages(1);
    init_thread(thread, name, default_prio);
    create_user_vaddr_bitmap(thread);
    thread_create(thread, start_process, filename);
    thread->pgdir = create_page_dir();
    block_desc_init(thread->u_block_desc);

    // 下面部分跟thread_start相同
    enum intr_status old_status = intr_disable();    // 关中断
    ASSERT(!elem_find(&thread_ready_list, &thread->general_tag));
    list_append(&thread_ready_list, &thread->general_tag);
    ASSERT(!elem_find(&thread_all_list, &thread->all_list_tag));
    list_append(&thread_all_list, &thread->all_list_tag);
    intr_set_status(old_status);    // 恢复之前的中断状态
}