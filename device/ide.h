#ifndef __DEVICE_IDE_H
#define __DEVICE_IDE_H
#include "stdint.h"
#include "sync.h"
#include "list.h"
#include "bitmap.h"

/* 分区结构 */
struct partition {
    uint32_t start_lba;          // 分区的起始扇区
    uint32_t sec_cnt;            // 分区容量扇区数
    struct disk* my_disk;        // 表示此分区属于哪个硬盘
    struct list_elem part_tag;   // 本分区的标记, 将来会将该分区汇总到队列中
    char name[8];                // 分区名称
    struct super_block* sb;      // 本分区的超级块
    struct bitmap block_bitmap;  // 块位图
    struct bitmap inode_bitmap;  // i节点位图
    struct list open_inodes;     // 本分区打开的i节点队列
};

struct disk {
    char name[8];                       // 本硬盘的名称, 如sda、sdb等
    struct ide_channel* my_channel;     // 用于表示此块硬盘插在那个通道上(primary? Secondary?)
    uint8_t dev_no;                     // 表示本硬盘是主盘还是从盘(0主1从)
    struct partition prim_parts[4];     // 本硬盘中的主分区数量, 最多4个
    struct partition logic_parts[8];    // 逻辑分区数量，理论是无限
};

/* ata通道结构(主板上有两个IDE插槽) */
struct ide_channel{
    char name[8];               // 本ata通道的名称, 如ata0或ide0
    uint16_t port_base;         // 本通道的端口基址
    uint8_t irq_no;             // 本通道所属的中断号, 在硬盘中断处理程序中要根据中断号来判断在哪个通道中操作
    struct lock lock;           // 本通道的锁
    bool expecting_intr;        // 表示本通道正在等待硬盘发出的中断
    struct semaphore disk_done; // 等待硬盘工作期间，用此信号量阻塞自己以让出cpu, 硬盘工作完成后会发出中断, 中断处理程序通过此信号量将驱动程序唤醒
    struct disk devices[2];     // 一个通道连接两个硬盘：一主一从
};

void intr_hd_handler(uint8_t irq_no);
void ide_init(void);
extern uint8_t channel_cnt;
extern struct ide_channel channels[];
extern struct list partition_list;
void ide_read(struct disk* hd, uint32_t lba, void* buf, uint32_t sec_cnt);
void ide_write(struct disk* hd, uint32_t lba, void* buf, uint32_t sec_cnt);
#endif