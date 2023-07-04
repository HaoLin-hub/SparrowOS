#include "dir.h"
#include "stdint.h"
#include "inode.h"
#include "file.h"
#include "fs.h"
#include "stdio-kernel.h"
#include "global.h"
#include "debug.h"
#include "memory.h"
#include "string.h"
#include "interrupt.h"
#include "super_block.h"

struct dir root_dir;    // 分区的根目录

/* 打开分区part的根目录 */
void open_root_dir(struct partition* part) {
    root_dir.inode = inode_open(part, part->sb->root_inode_no);
    root_dir.dir_pos = 0;
}

/* 在分区part上打开编号为inode_no的的目录文件并返回目录指针*/
struct dir* dir_open(struct partition* part, uint32_t inode_no){
    struct dir* pdir = (struct dir*)sys_malloc(sizeof(struct dir));
    pdir->inode = inode_open(part, inode_no);
    pdir->dir_pos = 0;
    return pdir;
}

/* 在part分区内的pdir目录内寻找名为name的文件或目录, 找到后返回true并将其目录项存入dir_e, 否则返回false */
bool search_dir_entry(struct partition* part, struct dir* pdir, const char* name, struct dir_entry* dir_e) {
    uint32_t block_cnt = 140;    // inode能容纳的总块数, 12个直接块 + 128个一级间接块(512 / 4)
    uint32_t* all_blocks = (uint32_t*)sys_malloc(48 + 512);  // 为这140个“块(扇区)地址”申请内存
    if(all_blocks == NULL) {
        printk("search_dir_entry: sys_malloc for all_blocks failed");
        return false;
    }

    // 将目录inode的i_blocks中的前12个扇区地址录入到all_blocks
    uint32_t block_idx = 0;
    while (block_idx < 12){
        all_blocks[block_idx] = pdir->inode->i_blocks[block_idx];
        block_idx++;
    }
	
    if(pdir->inode->i_blocks[12] != 0) { // 若含有一级间接块表, 从硬盘中的扇区地址/号(lba)i_blocks[12]处获取1扇区的数据
        ide_read(part->my_disk, pdir->inode->i_blocks[12], all_blocks + 12, 1);
    }
    // 至此, all_blocks存储的是该目录文件的所有扇区地址

    /* 写目录项的时候已保证目录项不跨扇区,
    * 这样读目录项时容易处理, 只申请容纳1个扇区的内存 */
    uint8_t* buf = (uint8_t*)sys_malloc(SECTOR_SIZE);
    struct dir_entry* p_de = (struct dir_entry*)buf;         // p_de为指向目录项的指针, 值为buf起始地址

    uint32_t dir_entry_size = part->sb->dir_entry_size;
    uint32_t dir_entry_cnt = SECTOR_SIZE / dir_entry_size;   // 1个扇区能容纳的目录项个数

    // 开始在目录文件的所有块中查找目录项
    block_idx = 0;
    while (block_idx < block_cnt){
        // 块地址为0时表示该块中无数据, 继续在其它块中查找
        if(all_blocks[block_idx] == 0){
            block_idx++;
            continue;
        }
        ide_read(part->my_disk, all_blocks[block_idx], buf, 1);
        uint32_t dir_entry_idx = 0;
        // 遍历当前块(扇区)中的所有目录项
        while (dir_entry_idx < dir_entry_cnt){
            // 若找到了"对应名字"的目录项, 就直接将其复制到dir_e中
            if(!strcmp(p_de->filename, name)){
                memcpy(dir_e, p_de, dir_entry_size);
                sys_free(buf);
                sys_free(all_blocks);
                return true;
            }
            dir_entry_idx++;
            p_de++;
        }
        block_idx++;
        p_de = (struct dir_entry*)buf;  // 此时p_de已经指向当前扇区内的最后一个完整目录项了, 需要恢复p_de为buf
        memset(buf, 0, SECTOR_SIZE);    // 将buf清0, 继续遍历下一个块(扇区)的目录项作准备
    }
    sys_free(buf);
    sys_free(all_blocks);
    return false;             // 在该目录文件中确实找不到相应的目录项
}

/* 关闭目录 */
void dir_close(struct dir* dir){
    // 根目录不能被关闭,否则还需要再次open_root_dir(), 且root_dir所在的内存是低端1MB内, 并非在堆中, free会出问题
    if(dir == &root_dir){
        // 不做任何处理直接返回
        return;
    }
    inode_close(dir->inode);
    sys_free(dir);
}

/* 在内存中初始化目录项p_de */
void create_dir_entry(char* filename, uint32_t inode_no, uint8_t file_type, struct dir_entry* p_de){
    ASSERT(strlen(filename) <= MAX_FILE_NAME_LEN);

    // 初始化目录项
    memcpy(p_de->filename, filename, strlen(filename));
    p_de->i_no = inode_no;
    p_de->f_type = file_type;
}

/* 将目录项p_de写入父目录文件parent_dir中, io_buf由主调函数提供 */
bool sync_dir_entry(struct dir* parent_dir, struct dir_entry* p_de, void* io_buf){
    struct inode* dir_inode = parent_dir->inode;
    uint32_t dir_size = dir_inode->i_size;                  // 目录文件大小, 即目录中所有目录项的大小之和
    uint32_t dir_entry_size = cur_part->sb->dir_entry_size; // 目录项大小

    ASSERT(dir_size % dir_entry_size == 0);               // 目录大小应该是目录项大小的整数倍
    uint32_t dir_entrys_per_sec = (512 / dir_entry_size); // 一个扇区可以容纳的目录项个数
    int32_t block_lba = -1;

    // 将该目录文件的所有扇区地址(12个直接块 + 128个间接块)存入all_blocks, 先初始化为0
    uint32_t all_blocks[140] = {0};
    // 将12个直接块(扇区)的地址存入all_blocks
    uint8_t block_idx = 0;
    while (block_idx < 12) {
        all_blocks[block_idx] = dir_inode->i_blocks[block_idx];
        block_idx++;
    }
    struct dir_entry* dir_e = (struct dir_entry*)io_buf;  // dir_e用来在io_buf中遍历目录项

    int32_t block_bitmap_idx = -1;
    // 开始遍历所有块以寻找目录项空位, 若已有扇区中没有空闲位, 在不超过文件大小的情况下申请新扇区来存储目录项
    block_idx = 0;
    while (block_idx < 140){
        // 文件最大支持 12个直接块 + 128个间接块 共140个块
        block_bitmap_idx = -1;
        if(all_blocks[block_idx] == 0){  // 先判断对应的块(扇区)未被分配, 以下三种情况下需要分配空闲块
            // 分配一个块(扇区), 并将扇区地址写入变量block_lba
            block_lba = block_bitmap_alloc(cur_part);
            if(block_lba == -1){
                printk("alloc block bitmap for sync_dir_entry failed\n");
                return false;
            }
            // 每分配一个块(扇区)就同步一次block_bitmap
            block_bitmap_idx = block_lba - cur_part->sb->data_start_lba; // 计算分配的块(扇区)是第几个块
            ASSERT(block_bitmap_idx != -1);
            bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

            block_bitmap_idx = -1;
            // 继续判断当前分配的块是直接块还是间接块
            if(block_idx < 12) {    // 若是直接块
                dir_inode->i_blocks[block_idx] = all_blocks[block_idx] = block_lba;
            } else if (block_idx == 12) {  // 若是尚未分配一级间接块"表"
                dir_inode->i_blocks[12] = block_lba;  // 上面分配的块用来存储一级间接块表的地址
                // 需要再分配一个块作为一级间接块表中的第一个间接块(真正存储目录项的地方)
                block_lba = -1;
                block_lba = block_bitmap_alloc(cur_part);
                if(block_lba == -1){  // 若分配失败需要执行回滚操作(即回收一级间接块表)
                    block_bitmap_idx = dir_inode->i_blocks[12] - cur_part->sb->data_start_lba;
                    bitmap_set(&cur_part->block_bitmap, block_bitmap_idx, 0);
                    dir_inode->i_blocks[12] = 0;
                    printk("alloc block bitmap for sync_dir_entry failed\n");
                    return false;
                }
                // 至此, 第一个间接块分配成功, 再次同步块位图
                block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
                ASSERT(block_bitmap_idx != -1);
                bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

                all_blocks[12] = block_lba;
                // 把新分配的间接块地址写入一级间接块"表"中
                ide_write(cur_part->my_disk, dir_inode->i_blocks[12], all_blocks + 12, 1);
            } else{
                // 若仅是间接块尚未分配, 那么就把该块指定为间接块
                all_blocks[block_idx] = block_lba;
                // 把新分配的第(block_idx - 12)个间接块地址写入一级间接块"表"中
                ide_write(cur_part->my_disk, dir_inode->i_blocks[12], all_blocks + 12, 1);
            }
            // !!! 再将新目录项p_de写入新分配的空闲块中 !!!
            memset(io_buf, 0, 512);
            memcpy(io_buf, p_de, dir_entry_size);
            ide_write(cur_part->my_disk, all_blocks[block_idx], io_buf, 1);
            dir_inode->i_size += dir_entry_size;
            return true;
        }

        // 若第block_idx个块早已存在, 就将其读入内存, 然后在该块中查找空目录项
        ide_read(cur_part->my_disk, all_blocks[block_idx], io_buf, 1);
        // 在扇区内查找空目录项
        uint8_t dir_entry_idx = 0;
        while (dir_entry_idx < dir_entrys_per_sec) {
            if((dir_e + dir_entry_idx)->f_type == FT_UNKNOWN) {  // FT_UNKNOWN为0, 初始化或是删除文件后,都会将f_type置为FT_UNKNOWN
                // 即找到了空目录项, 将目录项写入该空目录项, 并同步到硬盘
                memcpy(dir_e + dir_entry_idx, p_de, dir_entry_size);
                ide_write(cur_part->my_disk, all_blocks[block_idx], io_buf, 1);

                dir_inode->i_size += dir_entry_size;
                return true;
            }
            dir_entry_idx++;
        }
        block_idx++;
    }
    // 至此, 说明140个块中均满了, 无法填充目录项
    printk("directory is full!\n");
    return false;
}

/* 把分区part目录pdir中编号为inode_no的目录项删除 */
bool delete_dir_entry(struct partition* part, struct dir* pdir, uint32_t inode_no, void* io_buf) {
    struct inode* dir_inode = pdir->inode;
    uint32_t  all_blocks[140] = {0};
    /** step 1: 收集目录文件全部块(扇区)地址  **/
    uint32_t block_idx = 0;
    while (block_idx < 12) {
        all_blocks[block_idx] = dir_inode->i_blocks[block_idx];
        block_idx++;
    }
    if(dir_inode->i_blocks[12] != 0) {
        ide_read(part->my_disk, dir_inode->i_blocks[12], all_blocks + 12, 1);
    }

    // 目录项在存储时保证不会跨扇区
    uint32_t dir_entry_size = part->sb->dir_entry_size;
    uint32_t dir_entrys_per_sec = (SECTOR_SIZE / dir_entry_size);    // 每扇区最大的目录项数目

    struct dir_entry* dir_e = (struct dir_entry*)io_buf;    // 声明下面将用到的变量
    struct dir_entry* dir_entry_found = NULL;
    uint8_t dir_entry_idx, dir_entry_cnt;
    bool is_dir_first_block = false;    // 表示当前块(待删除的目录项所在的块)"是否为目录文件的第一个块“的标记

    /** step 2: 遍历所有块, 寻找目录项 **/
    block_idx = 0;
    while (block_idx < 140) {
        is_dir_first_block = false;
        if(all_blocks[block_idx] == 0){
            block_idx++;
            continue;
        }
        dir_entry_idx = dir_entry_cnt = 0;
        memset(io_buf, 0, SECTOR_SIZE);
        // 读取扇区, 获得目录项
        ide_read(part->my_disk, all_blocks[block_idx], io_buf, 1);

        // 遍历该块(扇区)中的所有的目录项, 统计该扇区的目录项数量以及是否有待删除的目录项
        while (dir_entry_idx < dir_entrys_per_sec) {
            // 只要目录项中的文件类型不是FT_UNKNOWN就表示该项有意义
            if((dir_e + dir_entry_idx)->f_type != FT_UNKNOWN) {
                if(!strcmp((dir_e + dir_entry_idx)->filename, ".")) {  // 若判断出目录项表示的文件的文件名为.则表示当前的块是目录文件的第一个块
                    is_dir_first_block = true;
                } else if (strcmp((dir_e + dir_entry_idx)->filename, ".") && strcmp((dir_e + dir_entry_idx)->filename, "..")){
                    // 否则说明当前块并非目录文件的第一个块, 那么就统计此块内的目录项个数, 用来判断删除目录项后是否应该回收该块
                    dir_entry_cnt++;
                    if((dir_e + dir_entry_idx)->i_no == inode_no) {  // 如果真在此块中的目录项中找到"要删除的文件对应的"目录项
                        ASSERT(dir_entry_found == NULL);
                        dir_entry_found = dir_e + dir_entry_idx;
                        // 仍需继续遍历该块, 统计总共的目录项数量
                    }
                }
            }
            dir_entry_idx++;
        }
        // 若此块(扇区)内未找到该目录项, 继续在下一个块中找
        if(dir_entry_found == NULL) {
            block_idx++;
            continue;
        }
        /** step 3: 如果在此扇区中找到了目录项后, 清除该目录项并判断是否回收扇区, 随后退出循环直接返回 **/
        ASSERT(dir_entry_cnt >= 1);
        if(dir_entry_cnt == 1 && !is_dir_first_block){  // 除目录文件的第1个块(扇区)外,若该扇区上只有该目录项自己,则将整个块(扇区)回收
            // (1): 在块位图中回收该块
            uint32_t block_bitmap_idx = all_blocks[block_idx] - part->sb->data_start_lba;
            bitmap_set(&part->block_bitmap, block_bitmap_idx, 0);
            bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

            // (2): 将块地址从数组i_blocks或一级间接块“表”中去掉
            if(block_idx < 12){
                dir_inode->i_blocks[block_idx] = 0;
            } else {    // 在一级间接块"表"中擦除该间接块地址
                // 统计间接块个数
                uint32_t indirect_blocks = 0;
                uint32_t indirect_block_idx = 12;
                while (indirect_block_idx < 140) {
                    if(all_blocks[indirect_block_idx] != 0){
                        indirect_blocks++;
                    }
                }
                ASSERT(indirect_blocks >= 1);
                if(indirect_blocks > 1) {  // 说明一级间接块“表”中还包括其他间接块, 仅在表中擦除当前这个间接块地址
                    all_blocks[block_idx] = 0;
                    ide_write(part->my_disk, dir_inode->i_blocks[12], all_blocks + 12, 1);
                } else {  // 说明一级间接块“表”中只有当前这个间接块,则直接把一级间接块“表”所在的块回收， 然后擦除间接块表的地址
                    /*** 将间接块“表”中的间接块表项对应的间接块也要回收掉，这是书中没有的 ***/
                    all_blocks[block_idx] = 0; //那个地方的块地址写成0
                    ide_write(part->my_disk, dir_inode->i_blocks[12], all_blocks+ 12, 1);

                    block_bitmap_idx = dir_inode->i_blocks[12] - part->sb->data_start_lba;
                    bitmap_set(&part->block_bitmap, block_bitmap_idx, 0);
                    bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
                    // 将间接块“表”地址清0
                    dir_inode->i_blocks[12] = 0;
                }
            }
        } else {  // 仅将该目录项内容清空
            memset(dir_entry_found, 0, dir_entry_size);
            ide_write(part->my_disk, all_blocks[block_idx], io_buf, 1);
        }
        // 更新目录文件的inode信息并同步到硬盘
        ASSERT(dir_inode->i_size >= dir_entry_size);
        dir_inode->i_size -= dir_entry_size;
        memset(io_buf, 0, SECTOR_SIZE * 2);
        inode_sync(part, dir_inode, io_buf);

        return true;
    }
    // 所有块中均位找到对应的目录项，则返回false，若出现这种情况应该是serarch_file出错了
    return false;
}

/* 读取目录, 读取成功返回"一个"目录项地址, 失败返回NULL */
struct dir_entry* dir_read(struct dir* dir) {
    struct dir_entry* dir_e = (struct dir_entry*)dir->dir_buf;  // 声明目录项指针dir_e指向目录结构内的512字节的缓冲区
    struct inode* dir_inode = dir->inode;

    // 将目录文件的所有块地址都收集到all_blocks数组中
    uint32_t all_blocks[140] = {0};
    uint32_t block_cnt = 12;

    uint32_t block_idx = 0, dir_entry_idx = 0;
    while (block_idx < 12){
        all_blocks[block_idx] = dir_inode->i_blocks[block_idx];
        block_idx++;
    }
    if(dir_inode->i_blocks[12] != 0){
        ide_read(cur_part->my_disk, dir_inode->i_blocks[12], all_blocks + 12, 1);
        block_cnt = 140;
    }
    // 开始遍历该目录的所有块, 在每个块中遍历目录项
    uint32_t cur_dir_entry_pos = 0;    // 当前目录项的偏移, 此项用来判断是否为 之前已经返回过的目录项, 结合P673场景理解一下, 其实每次调用该dir_read函数只返回未遍历过的最新的一个目录项
    uint32_t dir_entry_size = cur_part->sb->dir_entry_size;
    uint32_t dir_entrys_per_sec = SECTOR_SIZE / dir_entry_size;    // 1 扇区内可容纳的目录项个数

    block_idx = 0;
    while (block_idx < block_cnt){    // 一定要遍历所有块, 因为目录项的各自独立的, 可能因为删除掉n个目录项导致中间某块空了出来
        if(dir->dir_pos >= dir_inode->i_size) {  // 如果目录项偏移已经大于文件尺寸, 则说明遍历完了所有目录项
            return NULL;
        }
        if(all_blocks[block_idx] == 0) { // 此块为空块
            block_idx++;
            continue;
        }
        memset(dir_e, 0, SECTOR_SIZE);
        ide_read(cur_part->my_disk, all_blocks[block_idx], dir_e, 1);   // 将该块的所有内容(目录项)全读到dir_e缓冲区中
        // 进而遍历当前块(扇区)内的目录项
        dir_entry_idx = 0;
        while (dir_entry_idx < dir_entrys_per_sec) {
            if((dir_e + dir_entry_idx)->f_type != FT_UNKNOWN) {
                if (cur_dir_entry_pos < dir->dir_pos) {    // 判断当前目录项是否为最新的目录项, 避免返回曾经已经返回过的目录项
                    cur_dir_entry_pos += dir_entry_size;
                    dir_entry_idx++;
                    continue;
                }
                ASSERT(cur_dir_entry_pos == dir->dir_pos);
                dir->dir_pos += dir_entry_size;    // 更新为新位置, 即下一个该返回的目录项地址
                return dir_e + dir_entry_idx;
            }
            dir_entry_idx++;
        }
        block_idx++;
    }
    return NULL;
}

/* 判断目录是否为空 */
bool dir_is_empty(struct dir* dir) {
    struct inode* dir_inode = dir->inode;
    // 若目录下只有.和..两个目录项, 则表明目录为空
    return (dir_inode->i_size == cur_part->sb->dir_entry_size * 2);
}

/* 在父目录parent_dir中删除child_dir, 删除成功返回0 */
int32_t dir_remove(struct dir* parent_dir, struct dir* child_dir) {
    struct inode* child_dir_inode = child_dir->inode;
    // 我们要删除的子目录一定要是一个空目录, 而空目录只在inode->i_blocks[0]中有地址, 其它块均为空
    int32_t block_idx = 1;
    while (block_idx < 13) {
        ASSERT(child_dir_inode->i_blocks[block_idx] == 0);
        block_idx++;
    }

    void* io_buf = sys_malloc(SECTOR_SIZE * 2);
    if(io_buf == NULL){
        printk("dir_remove: malloc for io_buf failed\n");
        return -1;
    }

    // 在父目录parent_dir中删除子目录child_dir对应的目录项
    delete_dir_entry(cur_part, parent_dir, child_dir_inode->i_no, io_buf);
    // 回收子目录文件inode的i_blocks中所占用的块(扇区), 并同步到磁盘的inode_bitmap和block_bitmap
    inode_release(cur_part, child_dir_inode->i_no);
    sys_free(io_buf);
    return 0;
}