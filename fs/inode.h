#ifndef __FS_INODE_H
#define __FS_INODE_H
#include "stdint.h"
#include "list.h"
#include "ide.h"

/* inode结构 */
struct inode {
    uint32_t i_no;               // inode编号
    uint32_t i_size;             // 文件大小(指向目录时,是所有目录项大小之和)

    uint32_t i_open_cnts;        //记录此文件被打开的次数
    bool write_deny;             // 写文件不能并行写, 进程写文件前需要检查此标识

    uint32_t i_blocks[13];       // 0~11是直接块, 12用来存储一级间接块指针(注意本系统中: 一块 = 一扇区)
    struct list_elem inode_tag;  // 此inode的一个标志, 用于加入内存缓存中的“已打开inode队列”,避免下次打开同一文件时重复从较慢的磁盘中载入inode
};

struct inode* inode_open(struct partition* part, uint32_t inode_no);
void inode_sync(struct partition* part, struct inode* inode, void* io_buf);
void inode_init(uint32_t inode_no, struct inode* new_inode);
void inode_close(struct inode* inode);
void inode_release(struct partition* part, uint32_t inode_no);
void inode_delete(struct partition* part, uint32_t inode_no, void* io_buf);
#endif