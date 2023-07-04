#include "memory.h"
#include "bitmap.h"
#include "stdint.h"
#include "global.h"
#include "debug.h"
#include "print.h"
#include "string.h"
#include "sync.h"
#include "interrupt.h"


#define MEM_BITMAP_BASE 0xc009a000
/* 0xc0000000是内核从虚拟地址3G起, 0x100000意为跨过低端1MB内存，使虚拟地址在逻辑上连续, 即0xc0100000是堆的起始虚拟地址 */
#define K_HEAP_START 0xc0100000

#define PDE_IDX(addr) ((addr & 0xffc00000) >> 22)
#define PTE_IDX(addr) ((addr & 0x003ff000) >> 12)

/* 物理内存池结构, 用于支持生成两个实例用于管理内核物理内存池和用户物理内存池 */
struct pool {
    struct bitmap pool_bitmap;    // 本内存池用到的位图结构, 用于管理物理内存
    uint32_t phy_addr_start;      // 本内存池所管理的物理内存的起始地址
    uint32_t pool_size;           // 本内存池字节容量
    struct lock lock;             // 申请内存时互斥
};

/* 内存仓库 */
struct arena {
    struct mem_block_desc* desc;    // 此arena关联的mem_block_dec
    bool large;                     // large为true时, cnt表示页框数,否则表示空闲的mem_block数
    uint32_t cnt;
};
struct mem_block_desc k_block_descs[DESC_CNT]; // 内核的”内存块描述符“数组

struct pool kernel_pool, user_pool;    // 生成两个实例用于管理内核内存池和用户内存池
struct virtual_addr kernel_vaddr;      // 此结构用来给"内核"分配虚拟地址

/* 在pf表示的虚拟地址池中申请pg_cnt个虚拟页, 成功则返回虚拟页的起始地址, 失败则返回NULL */
static void* vaddr_get(enum pool_flags pf, uint32_t pg_cnt) {
    int vaddr_start = 0, bit_idx_start = -1;

    uint32_t cnt = 0;
    // 如果是在内核虚拟地址池中申请虚拟地址
    if(pf == PF_KERNEL) {
        bit_idx_start = bitmap_scan(&kernel_vaddr.vaddr_bitmap, pg_cnt);
        if(bit_idx_start == -1){
            return NULL;
        }
        // 运行到这里说明申请到了连续pg_cnt个虚拟地址, 那么就要将"从虚拟地址池中申请到的地址"在位图的对应的位置设为1
        while (cnt < pg_cnt){
            bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx_start + cnt, 1);
            cnt++;
        }
        // 要返回的"所申请到的这一大片连续虚拟地址"的起始地址
        vaddr_start = kernel_vaddr.vaddr_start + bit_idx_start * PG_SIZE;
    }else{
        // 用户进程虚拟地址池
        struct task_struct* cur = running_thread();
        bit_idx_start = bitmap_scan(&cur->userprog_vaddr.vaddr_bitmap, pg_cnt);
        if(bit_idx_start == -1){
            return NULL;
        }
        while (cnt < pg_cnt) {
            bitmap_set(&cur->userprog_vaddr.vaddr_bitmap, bit_idx_start + cnt, 1);
            cnt++;
        }
        // 返回申请到的连续虚拟地址空间的起始地址
        vaddr_start = cur->userprog_vaddr.vaddr_start + bit_idx_start * PG_SIZE;

        // 地址(0xc0000000 - PG_SIZE)作为用户3特权级栈已经在start_process被分配
        ASSERT((uint32_t)vaddr_start < (0xc0000000 - PG_SIZE));
    }
    return (void*)vaddr_start;
}

/* 获得虚拟地址vaddr对应的pte指针, 即该指针是一个“新的”虚拟地址, 但经转换后最终指向vaddr的pte所在的物理地址 */
uint32_t* pte_ptr(uint32_t vaddr) {
    // 先访问页目录表,即"新"虚拟地址的高10位是0xffc00000 ——> 再用vaddr页目录项的索引 作为"新"虚拟地址pte的索引 实际访问到虚拟地址vaddr对应的页表的物理地址 \
    // 最后 + vaddr的页表项索引 * 4 作为 "新"虚拟地址的物理页偏移量, 因此形成的"新"虚拟地址 转换后 可以访问到vaddr对应的页表项的物理地址
    uint32_t* pte = (uint32_t*)(0xffc00000 + ((vaddr & 0xffc00000) >> 10) + PTE_IDX(vaddr) * 4);
    return pte;
}

/* 得到虚拟地址vaddr对应的pde的指针 */
uint32_t* pde_ptr(uint32_t vaddr) {
    uint32_t* pde = (uint32_t*) ((0xfffff000) + PDE_IDX(vaddr) * 4);
    return pde;
}

/* 在m_pool指向的"物理内存池"中分配1个物理页, 成功则返回页框的物理地址, 失败则返回NULL */
static void* palloc(struct pool* m_pool){
    // 谨记：扫描或设置位图要保证原子操作
    int bit_idx = bitmap_scan(&m_pool->pool_bitmap, 1);
    if(bit_idx == -1){
        return NULL;
    }
    // 运行到这说明申请1个页面成功, 将其位图中对应的位置设为1
    bitmap_set(&m_pool->pool_bitmap, bit_idx, 1);
    uint32_t page_phyaddr = (m_pool->phy_addr_start + (bit_idx * PG_SIZE));

    return (void*) page_phyaddr;
}

/* 在页表中添加虚拟地址vaddr 与 物理地址page_phyaddr 的映射*/
static void page_table_add(void* _vaddr, void* _page_phyaddr){
    // 首先转换为32位地址
    uint32_t vaddr = (uint32_t)_vaddr, page_phyaddr = (uint32_t)_page_phyaddr;
    // 获得虚拟地址vaddr对应的页目录项pde 以及 页表项pte的"虚拟地址", 均保存在指针变量中
    uint32_t* pde = pde_ptr(vaddr);
    uint32_t* pte = pte_ptr(vaddr);

    // ！！！先在页目录内判断目录项的P位是否为1
    if(*pde & 0x00000001) {
        // 此时是要申请新的pte, 按理说pte应该是不存在的
        ASSERT(!(*pte & 0x00000001));

        if(!(*pte & 0x00000001)){    // 再次判断一下pte不存在
            *pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
        }else{ // 目前不会执行到这，因为上面的ASSERT会先执行
            PANIC("pte repeat");
            *pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
        }
    } else{    // 页目录项不存在, 所以要先创建页目录项再创建页表项
        uint32_t pde_phyaddr = (uint32_t) palloc(&kernel_pool); // 页目录表用到的页框一律从内核空间分配, 故申请一页内核物理页作为页表
        *pde = (pde_phyaddr | PG_US_U | PG_RW_W | PG_P_1);      // 设置好该页目录项的内容

        // 将分配到的页表物理页地址pde_phyaddr对应的物理内存清0, pte的高20位保留, 其余低12位为0, (pte & 0xfffff000)指向的是页表的"起始虚拟地址" 
        //  也就得到了刚刚申请的新物理页对应的虚拟地址（ 其实就是相当于将页表中的所有页表项均清0先）
        memset((void*)((int)pte & 0xfffff000), 0, PG_SIZE);
        ASSERT(!(*pte & 0x00000001));

        *pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);    // 设置vaddr对应的页表项(pte)的内容
    }
}

/* 分配pg_cnt个页空间, 成功则返回起始虚拟地址, 失败时返回NULL */
void* malloc_page(enum pool_flags pf, uint32_t pg_cnt) {
    ASSERT(pg_cnt > 0 && pg_cnt < 3840);
    /* 注意看：该函数的原理是三个动作的合成 */
    void* vaddr_start = vaddr_get(pf, pg_cnt);    // 1. 在虚拟地址池中申请连续pg_cnt页的虚拟地址, 返回这片虚拟地址的起始虚拟地址
    if(vaddr_start == NULL){
        return NULL;
    }

    uint32_t vaddr = (uint32_t)vaddr_start, cnt = pg_cnt;
    struct pool* mem_pool = pf & PF_KERNEL? &kernel_pool : &user_pool;    // 判断从哪个物理内存池中分配物理页
    // 因为虚拟地址是连续的，但物理地址可以是不连续的，所以逐一分别作映射
    while (cnt-- > 0){
        void* page_phyaddr = palloc(mem_pool);    // 2. 从相应的物理内存池中申请一个物理页
        if(page_phyaddr == NULL){
            /*... 这里省略了一下内容：失败时要将曾经已申请的虚拟地址和物理页全部回滚，在将来完成内存回收时再补充 */
            return NULL;
        }
        page_table_add((void*)vaddr, page_phyaddr);    // 3. 构建虚拟地址到物理地址的映射，即在页表中作映射(填充页表项)
        vaddr += PG_SIZE;    // 下一个虚拟页
    }
    return vaddr_start;
}

/* 从内核物理内存池中申请以页为单位的内存，成功则返回其起始虚拟地址，失败则返回NULL */
void* get_kernel_pages(uint32_t pg_cnt){
	lock_acquire(&kernel_pool.lock);
    void* vaddr = malloc_page(PF_KERNEL, pg_cnt);
    if(vaddr != NULL){    // 若分配的地址不为空, 将页框清0后返回
        memset(vaddr, 0, pg_cnt * PG_SIZE);
    }
	lock_release(&kernel_pool.lock);
	
    return vaddr;
}

/* 从用户物理内存池中申请以页为单位的内存，成功则返回其起始虚拟地址，失败则返回NULL*/
void* get_user_pages(uint32_t pg_cnt) {
    lock_acquire(&user_pool.lock);
    void* vaddr = malloc_page(PF_USER, pg_cnt);
    if(vaddr != NULL) {
        memset(vaddr, 0, pg_cnt * PG_SIZE);
    }
    lock_release(&user_pool.lock);
    return vaddr;
}

/* 可以指定虚拟地址vaddr与pf池中的物理地址关联(绑定到该物理地址), 仅支持一页空间分配 */
void* get_a_page(enum pool_flags pf, uint32_t vaddr){
    struct pool* mem_pool = pf & PF_KERNEL? &kernel_pool : &user_pool;
    lock_acquire(&mem_pool->lock);
    // 先将虚拟地址对应的位图置1
    struct task_struct* cur = running_thread();
    int32_t bit_idx = -1;

    // 当前是"用户进程"申请用户内存, 就修改用户进程自己的虚拟地址位图
    if(cur->pgdir != NULL && pf == PF_USER){
        bit_idx = (vaddr - cur->userprog_vaddr.vaddr_start) / PG_SIZE;
        ASSERT(bit_idx >= 0);
        bitmap_set(&cur->userprog_vaddr.vaddr_bitmap, bit_idx, 1);
    }
    // 如果是内核"线程"申请内核内存, 就修改kernel_vaddr
    else if(cur->pgdir == NULL && pf == PF_KERNEL){
        bit_idx = (vaddr - kernel_vaddr.vaddr_start) / PG_SIZE;
        ASSERT(bit_idx > 0);
        bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx, 1);
    }
    else{
        PANIC("get_a_page: not allow kernel alloc userspace or user alloc kernelspace by get_a_page");
    }
    // 分配一页物理内存
    void* page_phyaddr = palloc(mem_pool);
    if(page_phyaddr == NULL){
        lock_release(&mem_pool->lock);
        return NULL;
    }
    // 添加虚拟地址到物理内存的映射
    page_table_add((void*)vaddr, page_phyaddr);
    lock_release(&mem_pool->lock);

    return (void*)vaddr;
}

/* 专门针对fork时虚拟地址位图无需操作的情况：安装一页大小的vaddr, 但无需从虚拟内存地址池中设置位图 */
void* get_a_page_without_opvaddrbitmap(enum pool_flags pf, uint32_t vaddr) {
    struct pool* mem_pool = pf & PF_KERNEL? &kernel_pool : &user_pool;
    lock_acquire(&mem_pool->lock);
    void* page_phyaddr = palloc(mem_pool);
    if (page_phyaddr == NULL) {
        lock_release(&mem_pool->lock);
        return NULL;
    }
    // 添加虚拟地址到物理内存的映射
    page_table_add((void*)vaddr, page_phyaddr);
    lock_release(&mem_pool->lock);

    return (void*)vaddr;
}

/* 得到虚拟地址映射到的物理地址 */
uint32_t addr_v2p(uint32_t vaddr){
    // 获得虚拟地址vaddr对应的页表项所在的虚拟地址
    uint32_t* pte = pte_ptr(vaddr);
    // (*pte)的值就是页表项的内容, 高20位是虚拟地址vaddr对应的物理页框地址, 低12位是该物理页的属性
    return ((*pte & 0xfffff000) + (vaddr & 0x00000fff));
}

/* 初始化物理内存池 和 虚拟地址池, 根据内存容量all_mem的大小初始化物理内存池的相关结构 */
static void mem_pool_init(uint32_t all_mem){
    put_str("   mem_pool_init start\n");
    uint32_t page_table_size = PG_SIZE * 256;
    uint32_t used_mem = page_table_size + 0x100000;	  // 0x100000为低端1M内存

    uint32_t free_mem = all_mem - used_mem;
    // 1页为4k,不管总内存是不是4k的倍数,对于以页为单位的内存分配策略，不足1页的内存不用考虑了。
    uint16_t all_free_pages = free_mem / PG_SIZE;
    // 内核与用户各平分剩余内存
    uint16_t kernel_free_pages = all_free_pages / 2;
    uint16_t user_free_pages = all_free_pages - kernel_free_pages;

/* 为简化位图操作，余数不处理，坏处是这样做会丢内存。
好处是不用做内存的越界检查,因为位图表示的内存少于实际物理内存*/
    uint32_t kbm_length = kernel_free_pages / 8;			  // Kernel BitMap的长度,位图中的一位表示一页,以字节为单位
    uint32_t ubm_length = user_free_pages / 8;			  // User BitMap的长度.

    uint32_t kp_start = used_mem;				  // Kernel Pool start,内核内存池的起始地址
    uint32_t up_start = kp_start + kernel_free_pages * PG_SIZE;	  // User Pool start,用户内存池的起始地址

    kernel_pool.phy_addr_start = kp_start;
    user_pool.phy_addr_start   = up_start;

    kernel_pool.pool_size = kernel_free_pages * PG_SIZE;
    user_pool.pool_size	 = user_free_pages * PG_SIZE;

    kernel_pool.pool_bitmap.btmp_bytes_len = kbm_length;
    user_pool.pool_bitmap.btmp_bytes_len	  = ubm_length;


    // 内核使用的最高地址是0xc009f000,这是主线程的栈地址.(内核的大小预计为70K左右)
    // 32M内存占用的位图是1k.内核内存池的位图先定在MEM_BITMAP_BASE(0xc009a000)处.
    kernel_pool.pool_bitmap.bits = (void*)MEM_BITMAP_BASE;

    // 用户内存池的位图紧跟在内核内存池位图之后
    user_pool.pool_bitmap.bits = (void*)(MEM_BITMAP_BASE + kbm_length);
    /******************** 输出内存池信息 **********************/
    put_str("      kernel_pool_bitmap_start:");put_int((int)kernel_pool.pool_bitmap.bits);
    put_str(" kernel_pool_phy_addr_start:");put_int(kernel_pool.phy_addr_start);
    put_str("\n");
    put_str("      user_pool_bitmap_start:");put_int((int)user_pool.pool_bitmap.bits);
    put_str(" user_pool_phy_addr_start:");put_int(user_pool.phy_addr_start);
    put_str("\n");

    // 初始化内核内存池 和 用户内存池位图，置0
    bitmap_init(&kernel_pool.pool_bitmap);
    bitmap_init(&user_pool.pool_bitmap);
	
	lock_init(&kernel_pool.lock);
    lock_init(&user_pool.lock);
    // 初始化内核虚拟地址的位图,管理虚拟地址分配的情况，按实际物理内存大小生成数组，用于维护内核堆的虚拟地址,所以要和内核内存池大小一致
    kernel_vaddr.vaddr_bitmap.btmp_bytes_len = kbm_length;
    // 位图的数组指向一块未使用的内存,目前定位在内核内存池和用户内存池之外
    kernel_vaddr.vaddr_bitmap.bits = (void*)(MEM_BITMAP_BASE + kbm_length + ubm_length);
    kernel_vaddr.vaddr_start = K_HEAP_START;
    bitmap_init(&kernel_vaddr.vaddr_bitmap);

    put_str("   mem_pool_init done\n");
}
/****************************** 堆内存管理 *********************************/
/* 为malloc做准备 */
void block_desc_init(struct mem_block_desc* desc_array) {
    uint16_t desc_idx, block_size = 16;
    /* 初始化每个mem_block_desc描述符 */
    for(desc_idx = 0; desc_idx < DESC_CNT; desc_idx++){
        desc_array[desc_idx].block_size = block_size;
        desc_array[desc_idx].blocks_per_arena = (PG_SIZE - sizeof(struct arena)) / block_size;
        list_init(&desc_array[desc_idx].free_list);
        // 继续初始化下一个规格的mem_block_desc
        block_size *= 2;
    }
}

/* main退出时, 进程结束, 调用exit,根据物理页框地址pg_phy_addr在相应的物理内存池的位图清0, 不改动页表 */
void free_a_phy_page(uint32_t pg_phy_addr) {
    struct pool* mem_pool;
    uint32_t bit_idx = 0;
    if(pg_phy_addr >= user_pool.phy_addr_start) {    // 判断该物理地址属于用户物理内存池还是内核物理内存池
        mem_pool = &user_pool;
        bit_idx = (pg_phy_addr - user_pool.phy_addr_start) / PG_SIZE;
    } else {
        mem_pool = &kernel_pool;
        bit_idx = (pg_phy_addr - kernel_pool.phy_addr_start) / PG_SIZE;
    }
    bitmap_set(&mem_pool->pool_bitmap, bit_idx, 0);
}

/* 返回arena中第idx个内存块的地址 */
static struct mem_block* arena2block(struct arena* a, uint32_t idx){
    return (struct mem_block*) ((uint32_t)a + sizeof(struct arena) + idx * a->desc->block_size);
}

/* 返回内存块b所在的arena地址 */
static struct arena* block2arena(struct mem_block* b){
    return (struct arena*) ((uint32_t)b & 0xfffff000);
}

/* 在堆中申请size字节内存, 动态创建arena */
void* sys_malloc(uint32_t size) {
    enum pool_flags PF;
    struct pool* mem_pool;
    uint32_t pool_size;
    struct mem_block_desc* descs;
    struct task_struct* cur_thread = running_thread();

    // 判断一下用哪个内存池
    if(cur_thread->pgdir == NULL) {
        PF = PF_KERNEL;
        mem_pool = &kernel_pool;
        pool_size = kernel_pool.pool_size;
        descs = k_block_descs;
    }else{
        PF = PF_USER;
        mem_pool = &user_pool;
        pool_size = user_pool.pool_size;
        descs = cur_thread->u_block_desc;
    }

    // 若申请的内存不在内存池容量范围内则直接返回NULL
    if(size <= 0 || size >= pool_size){
        return NULL;
    }

    struct arena* a;
    struct mem_block* b;
    lock_acquire(&mem_pool->lock);    // 访问内存池需要加锁

    // 如果申请的内存超过内存块最大尺寸1024，则直接分配页框
    if(size > 1024) {
        uint32_t page_cnt = DIV_ROUND_UP(size + sizeof(struct arena), PG_SIZE);
        a = malloc_page(PF, page_cnt);
        if(a != NULL){
            memset(a, 0, page_cnt * PG_SIZE);    // 将分配的内存清0
        // 开始初始化arena的元信息
        a->desc = NULL;
        a->large = true;
        a->cnt = page_cnt;
        lock_release(&mem_pool->lock);
        return (void*)(a+1);
        }else{
            lock_release(&mem_pool->lock);
            return NULL;
        }
    }else{    // 否则就可以在各种规格的mem_block_desc中去适配
        uint8_t desc_idx;
        // 遍历内存块描述符，从中找出匹配申请最合适的内存块“规格”
        for(desc_idx = 0; desc_idx < DESC_CNT; desc_idx++){
            if(size <= descs[desc_idx].block_size) break;    // 找到后就退出
        }
        // 若desc_idx所指向的mem_block_desc的free_list中已没有可用的mem_block, 就创建新的arena提供mem_block
        if(list_empty(&descs[desc_idx].free_list)){
            a = malloc_page(PF, 1);   // 拓展一页框作为arena
            if(a == NULL){
                lock_release(&mem_pool->lock);
                return NULL;
            }
            memset(a, 0, PG_SIZE);
            // 对于新创建的arena, 将desc置为相应的内存块描述符
            a->desc = &descs[desc_idx];
            a->large = false;
            a->cnt = descs[desc_idx].blocks_per_arena;

            // 开始要将arena拆分成内存块, 并添加到内存描述符的free_list中
            uint32_t block_idx;
            enum intr_status old_status = intr_disable();    // 关中断, 保证原子操作

            for(block_idx = 0; block_idx < descs[desc_idx].blocks_per_arena; block_idx++){
                b = arena2block(a, block_idx);    // 拆分出第block_idx块内存块
                ASSERT(!elem_find(&a->desc->free_list, &b->free_elem));
                list_append(&a->desc->free_list, &b->free_elem);
            }
            intr_set_status(old_status);    // 恢复中断状态
        }
        // 走到这步, 即已经有内存块可供分配
        b = elem2entry(struct mem_block, free_elem, list_pop(&(descs[desc_idx].free_list))); // 从链表中弹出的是mem_block的free_elem的地址
        memset(b, 0, descs[desc_idx].block_size);

        a = block2arena(b);  // 获取内存块b所在的arena的地址
        a->cnt--;            // 表示此arena中的空闲内存块数-1
        lock_release(&mem_pool->lock);

        return (void*)b; // 返回分配的内存块
    }
}
/***************************** 内存释放 **********************************/
/* 将物理地址pg_phy_addr回收到物理内存池 */
void pfree(uint32_t pg_phy_addr) {
    struct pool* mem_pool;
    uint32_t bit_idx = 0;
    if(pg_phy_addr >= user_pool.phy_addr_start) {
        mem_pool = &user_pool;
        bit_idx = (pg_phy_addr - mem_pool->phy_addr_start) / PG_SIZE; // 第几个物理页
    }else{
        mem_pool = &kernel_pool;
        bit_idx = (pg_phy_addr - mem_pool->phy_addr_start) / PG_SIZE; // 第几个物理页
    }
    bitmap_set(&mem_pool->pool_bitmap, bit_idx, 0);    // 将位图中的该位清0
}

/* 去掉页表中虚拟地址vaddr的映射, 注意只去掉vaddr对应的pte */
static void page_table_pte_remove(uint32_t vaddr) {
    uint32_t* pte = pte_ptr(vaddr);    // 得到该虚拟地址vaddr对应的页表项的"虚拟地址"
    *pte &= ~PG_P_1;    // 将页表项pte的P位置0
    asm volatile ("invlpg %0" : : "m"(vaddr) : "memory");
}

/* 在虚拟地址池中释放以_vaddr起始的连续pg_cnt个虚拟页地址 */
static void vaddr_remove(enum pool_flags pf, void* _vaddr, uint32_t pg_cnt) {
    uint32_t bit_idx_start = 0, vaddr = (uint32_t)_vaddr, cnt = 0;

    // 先判断一下处理哪个虚拟内存池
    if(pf == PF_KERNEL) {
        bit_idx_start = (vaddr - kernel_vaddr.vaddr_start) / PG_SIZE;
        while (cnt < pg_cnt) {
            bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx_start + cnt, 0);
            cnt++;
        }
    }else{    // 用户虚拟内存池
        struct task_struct* cur_thread = running_thread();
        bit_idx_start = (vaddr - cur_thread->userprog_vaddr.vaddr_start) / PG_SIZE;
        while (cnt < pg_cnt) {
            bitmap_set(&cur_thread->userprog_vaddr.vaddr_bitmap, bit_idx_start + cnt, 0);
            cnt++;
        }
    }
}

/* 释放以虚拟地址vaddr为起始的"pg_cnt个物理页框" */
void mfree_page(enum pool_flags pf, void* _vaddr, uint32_t pg_cnt){
    uint32_t vaddr = (uint32_t)_vaddr, page_cnt = 0;
    ASSERT(pg_cnt >= 1 && vaddr % PG_SIZE == 0);
    // 获取虚拟地址对应的物理地址
    uint32_t pg_phy_addr =  addr_v2p(vaddr);
    // 确保待释放的物理内存位于(低端1MB + 1K大小的页目录+1K大小的页表)地址范围外
    ASSERT((pg_phy_addr % PG_SIZE) == 0 && pg_phy_addr >= 0x102000);

    // 下面开始正式工作,先判断一下是从哪个物理内存池中释放页框
    if(pg_phy_addr >= user_pool.phy_addr_start){ // 位于用户物理内存池
        // 先减PG_SIZE以便处理while循环的边界条件
        vaddr -= PG_SIZE;
        while (page_cnt < pg_cnt){
            vaddr += PG_SIZE;
            pg_phy_addr = addr_v2p(vaddr);
            //确保物理地址属于用户物理内存池
            ASSERT((pg_phy_addr % PG_SIZE) == 0 && pg_phy_addr >= user_pool.phy_addr_start);
            // 1. 先将对应的物理页框归还到内存池
            pfree(pg_phy_addr);
            // 2. 再从页表中清除此虚拟地址所在的页表项pte
            page_table_pte_remove(vaddr);

            page_cnt++;
        }
        // 3. 清空虚拟地址位图中以_vaddr为起始虚拟地址的连续pg_cnt个位
        vaddr_remove(pf, _vaddr, pg_cnt);
    }else{  // 位于内核物理内存池
        vaddr -= PG_SIZE;
        while (page_cnt < pg_cnt){
            vaddr += PG_SIZE;
            pg_phy_addr = addr_v2p(vaddr);
            // 确保待释放的物理内存只属于内核物理内存池
            ASSERT((pg_phy_addr % PG_SIZE == 0) && pg_phy_addr >= kernel_pool.phy_addr_start && pg_phy_addr < user_pool.phy_addr_start);
            pfree(pg_phy_addr);
            page_table_pte_remove(vaddr);

            page_cnt++;
        }
        vaddr_remove(pf, _vaddr, pg_cnt);
    }
}

/* 回收内存ptr的统一接口 */
void sys_free(void* ptr) {
    ASSERT(ptr != NULL);
    if(ptr != NULL){
        enum pool_flags PF;
        struct pool* mem_pool;    // 物理内存池结构体指针

        // 判断调用本函数的是内核线程还是用户进程
        if(running_thread()->pgdir == NULL){  // 线程调用的
            ASSERT((uint32_t)ptr >= K_HEAP_START);
            PF = PF_KERNEL;
            mem_pool = &kernel_pool;
        }else{
            PF = PF_USER;
            mem_pool = &user_pool;
        }
        lock_acquire(&mem_pool->lock);    // 访问内存池资源需先加锁
        // 获取ptr所指向的内存块所在的arena指针, 进而获取arena的元信息
        struct mem_block* b = ptr;
        struct arena* a = block2arena(b);  // 获得该内存块对应的arena(b只是该arena中的某个内存块罢了)
        ASSERT(a->large == 0 || a->large == 1);
        if(a->desc == NULL && a->large == true) {  // 说明待释放的内存(也就是ptr指向的内存)并不是在arena中的小内存块，而是大于1024字节的大内存，即>=1个页框，页框数量由元信息决定
            mfree_page(PF, a, a->cnt);
        }else{  // 小于1024的小内存块
            list_append(&a->desc->free_list, &b->free_elem);  // 先将内存块回收到arena对应的"内存块描述符“free_list中
            (a->cnt)++;

            // 再判断此arena中的内存块是否都是空闲, 如果是就释放arena
            if(a->cnt == a->desc->blocks_per_arena) {
                uint32_t block_idx;
                for(block_idx = 0; block_idx < a->desc->blocks_per_arena; block_idx++) {
                    struct mem_block* b = arena2block(a, block_idx);
                    ASSERT(elem_find(&a->desc->free_list, &b->free_elem));
                    list_remove(&b->free_elem);
                }
                mfree_page(PF, a, 1);
            }
        }
        lock_release(&mem_pool->lock);
    }
}

/* 内存管理部分初始化入口 */
void mem_init() {
    put_str("mem_init start\n");
    uint32_t mem_bytes_total = (*(uint32_t*)(0xb00));
    // 初始化内存池
    mem_pool_init(mem_bytes_total);
    // 初始化mem_block_desc数组descs,为malloc做准备
    block_desc_init(k_block_descs);
    put_str("mem_init done\n");
}