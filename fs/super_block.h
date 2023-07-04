#ifndef __FS_SUPER_BLOCK_H
#define __FS_SUPER_BLOCK_H
#include "stdint.h"

/* 超级块 */
struct super_block {
    uint32_t magic;            // 魔数, 用来表示文件系统类型, 支持多文件系统的OS通过此标志来识别文件系统类型
    uint32_t sec_cnt;          // 本分区总共的扇区数
    uint32_t inode_cnt;        // 本分区中的inode数量
    uint32_t part_lba_base;    // 本分区的起始lba地址

    uint32_t block_bitmap_lba;   // 空闲块位图本身的起始扇区地址
    uint32_t block_bitmap_sects; // 空闲块位图本身占用的扇区数量

    uint32_t inode_bitmap_lba;   // inode位图本身的起始扇区地址
    uint32_t inode_bitmap_sects; // inode位图本身占用的扇区数量

    uint32_t inode_table_lba;    // inode数组起始扇区地址
    uint32_t inode_table_sects;  // inode数组占用的扇区数量

    uint32_t data_start_lba;     // 数据区开始的第一个扇区地址

    uint32_t root_inode_no;      // 根目录所在的inode编号
    uint32_t dir_entry_size;     // 目录项大小

    uint8_t pad[460];            // 填充460字节凑足整个结构体为512字节大小
} __attribute__ ((packed));

#endif