#include "inode.h"
#include "fs.h"
#include "file.h"
#include "global.h"
#include "debug.h"
#include "memory.h"
#include "interrupt.h"
#include "list.h"
#include "stdio-kernel.h"
#include "string.h"
#include "super_block.h"

/* 用于存储(定位)inode位置 */
struct inode_position {
    bool two_sec;      // inode是否跨扇区
    uint32_t sec_lba;  // inode所在的(绝对)扇区号
    uint32_t off_size; // inode在扇区内的字节偏移量
};

/* 获取inode所在的扇区和扇区内的偏移量, 将其写入inode_pos中 */
static void inode_locate(struct partition* part, uint32_t inode_no, struct inode_position* inode_pos) {
    ASSERT(inode_no < 4096);
    // inode_table在磁盘上是连续的
    uint32_t inode_table_lba = part->sb->inode_table_lba;
    uint32_t inode_size = sizeof(struct inode);
    uint32_t off_size = inode_no * inode_size;    // 第inode_no号i节点相对于inode_table_lba的字节偏移量
    uint32_t off_sec = off_size / 512;            // 第inode_no号i节点相对于inode_table_lba的"扇区偏移量"
    uint32_t off_size_in_sec = off_size % 512;    // 第inode_no号i节点在"其所在扇区“的偏移量

    // 判断此i节点是否跨越了2个扇区
    uint32_t left_in_sec = 512 - off_size_in_sec;
    if(left_in_sec < inode_size) {
        // 若扇区中剩下的空间不足以容纳一个inode, 必然是i节点跨越了2个扇区
        inode_pos->two_sec = true;
    }else{
        inode_pos->two_sec = false;
    }
    inode_pos->sec_lba = inode_table_lba + off_sec;
    inode_pos->off_size = off_size_in_sec;
}

/* 将inode写入到磁盘分区part */
void inode_sync(struct partition* part, struct inode* inode, void* io_buf) { // io_buf是主调函数提前申请好的用于硬盘io的缓冲区
    uint8_t inode_no = inode->i_no;
    struct inode_position inode_pos;
    inode_locate(part, inode_no, &inode_pos);  // 将inode位置信息存入临时变量inode_pos中
    ASSERT(inode_pos.sec_lba <= (part->start_lba + part->sec_cnt));

    // inode中的成员inode_tag和i_open_cnts, write_deny用于记录inode被操作的状态,只在内存中有意义, 写入硬盘前将其清除掉
    struct inode pure_inode;
    memcpy(&pure_inode, inode, sizeof(struct inode));
    pure_inode.i_open_cnts = 0;
    pure_inode.write_deny = false;
    pure_inode.inode_tag.prev = pure_inode.inode_tag.next = NULL;

    char* inode_buf = (char*)io_buf;
    if(inode_pos.two_sec) {
        // 若inode跨越了两个扇区, 就要读出两个扇区再写入两个扇区
        ide_read(part->my_disk, inode_pos.sec_lba, inode_buf, 2);  // 写入的数据小于一扇区, 要将原硬盘上的内容先读出来再和新数据拼接后再写入

        // 开始将待写入的inode拼入这2个扇区中的相应位置
        memcpy((inode_buf + inode_pos.off_size), &pure_inode, sizeof(struct inode));
        // 将拼接好后的数据(2个扇区)写入磁盘
        ide_write(part->my_disk, inode_pos.sec_lba, inode_buf, 2);
    } else{
        // 若没有跨扇区
        ide_read(part->my_disk, inode_pos.sec_lba, inode_buf, 1);
        memcpy((inode_buf + inode_pos.off_size), &pure_inode, sizeof(struct inode));
        ide_write(part->my_disk, inode_pos.sec_lba, inode_buf, 1);
    }
}

/* 根据i结点编码 返回相应的i结点 */
struct inode* inode_open(struct partition* part, uint32_t inode_no){
    // 先在"已打开的inode链表"中查找对应的inode, 此链表是为提速创建的内存缓存
    struct list_elem* elem = part->open_inodes.head.next;    // 头节点
    struct inode* inode_found;

    while (elem != &part->open_inodes.tail) {
        inode_found = elem2entry(struct inode, inode_tag, elem);
        if(inode_found->i_no == inode_no) {
            inode_found->i_open_cnts++;
            return inode_found;
        }
        elem = elem->next;
    }

    // 由于在open_inodes链表中找不到, 就从硬盘中读入此inode并加入到此链表
    struct inode_position inode_pos;
    inode_locate(part, inode_no, &inode_pos); // 将inode位置信息存入inode_pos中

    /* 为使通过sys_malloc创建的新inode被所有任务共享, 需将inode至于内核空间, 故需要临时将cur_pcb->pgdir置为NULL */
    struct task_struct* cur = running_thread();
    uint32_t * cur_pagedir_bak = cur->pgdir;  // 将当前任务(无论进程还是线程统一处理, 不再进行额外判断)的页目录表地址备份到变量cur_pagedir_bak中
    cur->pgdir  = NULL;

    // 上面三行代码完成后, 下面分配的内存将位于"内核区"
    inode_found = (struct inode*)sys_malloc(sizeof(struct inode));
    // 恢复pgdir
    cur->pgdir = cur_pagedir_bak;

    char* inode_buf;
    if(inode_pos.two_sec){    // 考虑跨扇区的情况
        inode_buf = (char*)sys_malloc(1024); // 动态申请2个扇区大小的内存给inode_buf
        ide_read(part->my_disk, inode_pos.sec_lba, inode_buf, 2);
    } else{  // 否则所查找的inode未跨扇区, 一个扇区大小的缓冲区足够
        inode_buf = (char*)sys_malloc(512);
        ide_read(part->my_disk, inode_pos.sec_lba, inode_buf, 1);
    }
    // 此时inode_buf中是完整的inode_no号i节点的内容
    memcpy(inode_found, inode_buf + inode_pos.off_size, sizeof(struct inode)); // 将扇区中的inode内容复制到inode_found中

    // 因为一会可能还是要用到该inode，故将其插入到队首便于提前检索到
    list_push(&part->open_inodes, &inode_found->inode_tag);
    inode_found->i_open_cnts = 1;

    sys_free(inode_buf);
    return inode_found;
}

/* 关闭inode or 减少inode的打开数 */
void inode_close(struct inode* inode){
    // 若没有进程再打开此文件, 将此inode去掉并释放空间
    enum intr_status old_status = intr_disable();
    if(--inode->i_open_cnts == 0){
        list_remove(&inode->inode_tag);  // 将i节点从part->open_inodes列表中去掉
        // inode_open时为实现inode被所有进程共享,已经在sys_malloc为inode分配了内核空间,释放inode时也要确保释放的是内核内存池
        struct task_struct* cur = running_thread();
        uint32_t* cur_pagedir_bak = cur->pgdir;
        cur->pgdir = NULL;
        sys_free(inode);
        cur->pgdir = cur_pagedir_bak;
    }
    intr_set_status(old_status);
}

/* 将硬盘分区part上指定的inode清空 */
void inode_delete(struct partition* part, uint32_t inode_no, void* io_buf) {
    ASSERT(inode_no < 4096);
    struct inode_position inode_pos;
    inode_locate(part, inode_no, &inode_pos);  // 将inode_no号节点的位置信息存储inode_pos中
    ASSERT(inode_pos.sec_lba <= (part->start_lba + part->sec_cnt));

    char* inode_buf = (char*)io_buf;
    if(inode_pos.two_sec) {  // inode跨扇区, 读入2个扇区的内容到inode_buf
        ide_read(part->my_disk, inode_pos.sec_lba, inode_buf, 2);
        // 清零操作
        memset((inode_buf + inode_pos.off_size), 0, sizeof(struct inode));
        // 同步回磁盘
        ide_write(part->my_disk, inode_pos.sec_lba, inode_buf, 2);
    } else {
        ide_read(part->my_disk, inode_pos.sec_lba, inode_buf, 1);
        // 将inode_buf的inode相应位置清0
        memset((inode_buf + inode_pos.off_size), 0, sizeof(struct inode));
        // 同步回磁盘
        ide_write(part->my_disk, inode_pos.sec_lba, inode_buf, 1);
    }
}

/* 回收inode的数据块和inode本身在inode_bitmap中的bit */
void inode_release(struct partition* part, uint32_t inode_no) {
    // 获取要删除的inode
    struct inode* inode_to_del = inode_open(part, inode_no);
    ASSERT(inode_to_del->i_no == inode_no);

    /** Step 1 : 回收inode占用的所有块  **/
    uint8_t block_idx = 0, block_cnt = 12;
    uint32_t block_bitmap_idx;
    uint32_t all_blocks[140] = {0};  // 用all_blocks来存储inode对应的文件占用的所有块(扇区)地址

    while (block_idx < 12) {
        all_blocks[block_idx] = inode_to_del->i_blocks[block_idx];
        block_idx++;
    }
    // 若一级间接块"表"存在, 将其128个间接块(扇区)地址存入all_blocks
    if(inode_to_del->i_blocks[12] != 0){
        ide_read(part->my_disk, inode_to_del->i_blocks[12], all_blocks + 12, 1);
        block_cnt = 140;  // 更新inode对应的文件占用的块的数量

        // 回收一级间接块“表”占用的扇区
        block_bitmap_idx = inode_to_del->i_blocks[12] - part->sb->data_start_lba;
        ASSERT(block_bitmap_idx > 0);
        bitmap_set(&part->block_bitmap, block_bitmap_idx, 0);
        bitmap_sync(part, block_bitmap_idx, BLOCK_BITMAP);    // 同步回磁盘
    }
    // inode所有块地址已经收集到all_blocks中, 下面逐个回收
    block_idx = 0;
    while (block_idx < block_cnt) {
        if(all_blocks[block_idx] != 0){  // 块地址为0的话说明该块未分配(这里注意inode对应的文件是“普通文件”还是“目录文件”)
            block_bitmap_idx = 0;
            block_bitmap_idx = all_blocks[block_idx] - part->sb->data_start_lba;
            ASSERT(block_bitmap_idx > 0);
            bitmap_set(&part->block_bitmap, block_bitmap_idx, 0);
            bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
        }
        block_idx++;
    }

    /** Step 2 : 回收该inode所占用的inode_bitmap中的位 **/
    bitmap_set(&part->inode_bitmap, inode_no, 0);
    bitmap_sync(cur_part, inode_no, INODE_BITMAP);

    /******     以下inode_delete是调试用的    ******
   * 此函数会在inode_table中将此inode清0,
   * 但实际上是不需要的,inode分配是由inode位图控制的,
   * 硬盘上的数据不需要清0,可以直接覆盖*/
    void* io_buf = sys_malloc(1024);
    inode_delete(part, inode_no, io_buf);
    sys_free(io_buf);
    /***********************************************/
    inode_close(inode_to_del);
}

/* 初始化new_inode */
void inode_init(uint32_t inode_no, struct inode* new_inode) {
    new_inode->i_no = inode_no;
    new_inode->i_size = 0;
    new_inode->i_open_cnts = 0;
    new_inode->write_deny = false;

    // 初始化块索引数组i_block
    uint8_t block_idx = 0;
    while (block_idx < 13) {
        // i_blocks[0~11]为直接块地址, i_blocks[12]为一级间接块地址, 在此统统置为0
        new_inode->i_blocks[block_idx] = 0;
        block_idx++;
    }
}