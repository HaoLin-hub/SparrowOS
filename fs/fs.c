#include "fs.h"
#include "super_block.h"
#include "inode.h"
#include "dir.h"
#include "stdint.h"
#include "stdio-kernel.h"
#include "list.h"
#include "string.h"
#include "ide.h"
#include "global.h"
#include "debug.h"
#include "memory.h"
#include "file.h"
#include "console.h"
#include "keyboard.h"
#include "ioqueue.h"
#include "pipe.h"

struct partition* cur_part;	 // 默认情况下操作的是哪个分区

/* list_traversal的回调函数：在分区链表中找到名为part_name的分区,并将其指针赋值给cur_part */
static bool mount_partition(struct list_elem* pelem, int arg) {
    char* part_name = (char*)arg;    // 将arg还原成字符指针, 指向要操作的分区名字(sda1, sda2...)
    struct partition* part = elem2entry(struct partition, part_tag, pelem);

    if(!strcmp(part->name, part_name)) {    // 比对两者, 看是否找到了arg对应名字的分区(若两个字符串相等,strcmp函数返回0)

        cur_part = part;                     // 将该分区的指针赋给cur_part
        struct disk* hd = cur_part->my_disk; // 获取该分区所属的硬盘

        // 动态申请一扇区大小的内存用于缓存从硬盘上读入的超级块
        struct super_block* sb_buf = (struct super_block*)sys_malloc(SECTOR_SIZE);

        // 在内存中创建"要操作的分区"cur_part的超级块
        cur_part->sb = (struct super_block*)sys_malloc(sizeof(struct super_block));
        if(cur_part->sb == NULL){
            PANIC("alloc memory failed!");
        }

        // (1) 从要操作的分区所属的硬盘中读入超级块的内容到缓存sb_buf中
        memset(sb_buf, 0, SECTOR_SIZE);
        ide_read(hd, cur_part->start_lba + 1, sb_buf, 1);
        memcpy(cur_part->sb, sb_buf, sizeof(struct super_block)); // 复制到分区结构的超级块中

        //(2) 从硬盘上读入块位图到分区的block_bitmap.bits中
        cur_part->block_bitmap.bits = (uint8_t*)sys_malloc(sb_buf->block_bitmap_sects * SECTOR_SIZE);// 为要操作的分区cur_part的块位图申请内存
        if(cur_part->block_bitmap.bits == NULL) {
            PANIC("alloc memory failed!");
        }
        cur_part->block_bitmap.btmp_bytes_len = sb_buf->block_bitmap_sects * SECTOR_SIZE;
        ide_read(hd, sb_buf->block_bitmap_lba, cur_part->block_bitmap.bits, sb_buf->block_bitmap_sects);

        // (3) 从硬盘上读入inode位图到分区的inode_bitmap.bits
        cur_part->inode_bitmap.bits = (uint8_t*)sys_malloc(sb_buf->inode_bitmap_sects * SECTOR_SIZE);
        if(cur_part->inode_bitmap.bits == NULL) {
            PANIC("alloc memory failed!");
        }
        cur_part->inode_bitmap.btmp_bytes_len = sb_buf->inode_bitmap_sects * SECTOR_SIZE;
        ide_read(hd, sb_buf->inode_bitmap_lba, cur_part->inode_bitmap.bits, sb_buf->inode_bitmap_sects);

        list_init(&cur_part->open_inodes);
        printk("mount %s done!\n", part->name);

        // 此处返回true是为了迎合主调函数list_traversal的实现,与函数本身功能无关,只有返回true时list_traversal才会停止遍历,减少了后面元素无意义的遍历
        return true;
    }
    return false;  // 使用list_traversal继续遍历, 继续找到名字为(char*)arg的分区
}

/* 格式化分区, 也就是初始化分区的元信息, 创建文件系统 */
static void partition_format(struct partition* part) {
     uint32_t boot_sector_sects = 1;    // 为引导块和超级块占用的扇区数赋值
     uint32_t super_block_sects = 1;

     uint32_t inode_bitmap_sects = DIV_ROUND_UP(MAX_FILES_PER_PART, BITS_PER_SECTOR);    // inode位图所占扇区数(一个bit代表一个inode)  【1】
     uint32_t inode_table_sects = DIV_ROUND_UP((sizeof(struct inode) * MAX_FILES_PER_PART), SECTOR_SIZE); // inode数组所占的扇区数   【200】

     uint32_t used_sects = boot_sector_sects + super_block_sects + inode_bitmap_sects + inode_table_sects;
     uint32_t free_sects = part->sec_cnt - used_sects;    // 计算出空闲块的个数                                                      【163093】

     // 开始计算空闲块位图占用的扇区数
     uint32_t block_bitmap_sects = DIV_ROUND_UP(free_sects, BITS_PER_SECTOR);    // 空闲块个数 除以 每个扇区的bit数                    【40】
     uint32_t block_bitmap_bit_len = free_sects - block_bitmap_sects;            // block_bitmap_bit_len是位图中位的长度(bit数量), 也是"真正的"空闲块的数量  【162775】
     block_bitmap_sects = DIV_ROUND_UP(block_bitmap_bit_len, BITS_PER_SECTOR);   // 空闲块位图最终占用的扇区数                         【40】

     // 构建超级块, 并初始化
     struct super_block sb;
     sb.magic = 0x19980924;
     sb.sec_cnt = part->sec_cnt;
     sb.inode_cnt = MAX_FILES_PER_PART;
     sb.part_lba_base = part->start_lba;

     sb.block_bitmap_lba = sb.part_lba_base + 2;    // 第0块是引导块, 第1块是超级块
     sb.block_bitmap_sects = block_bitmap_sects;

     sb.inode_bitmap_lba = sb.block_bitmap_lba + sb.block_bitmap_sects;
     sb.inode_bitmap_sects = inode_bitmap_sects;

     sb.inode_table_lba = sb.inode_bitmap_lba + sb.inode_bitmap_sects;
     sb.inode_table_sects = inode_table_sects;

     sb.data_start_lba = sb.inode_table_lba + sb.inode_table_sects;
     sb.root_inode_no = 0;
     sb.dir_entry_size = sizeof(struct dir_entry);

   printk("%s info:\n", part->name);
   printk("   magic:0x%x\n   part_lba_base:0x%x\n   all_sectors:0x%x\n   inode_cnt:0x%x\n   block_bitmap_lba:0x%x\n   block_bitmap_sectors:0x%x\n   inode_bitmap_lba:0x%x\n   inode_bitmap_sectors:0x%x\n   inode_table_lba:0x%x\n   inode_table_sectors:0x%x\n   data_start_lba:0x%x\n", sb.magic, sb.part_lba_base, sb.sec_cnt, sb.inode_cnt, sb.block_bitmap_lba, sb.block_bitmap_sects, sb.inode_bitmap_lba, sb.inode_bitmap_sects, sb.inode_table_lba, sb.inode_table_sects, sb.data_start_lba);

     struct disk* hd = part->my_disk;    // 获取当前分区part自己所属的硬盘hd

     /** 1. 将超级块写入本分区的1扇区 **/
     ide_write(hd, part->start_lba + 1, &sb, 1);
     printk("   super_block_lba:0x%x\n", part->start_lba + 1);

     // 因”inode数组和空闲块位图“所占用的扇区数较多, 无法用局部变量来作为缓存(从而写入硬盘), 故找出数据量最大的元信息, 用其尺寸做存储缓冲区
     uint32_t buf_size = (sb.block_bitmap_sects >= sb.inode_bitmap_sects? sb.block_bitmap_sects : sb.inode_bitmap_sects);
     buf_size = (buf_size >= sb.inode_table_sects? buf_size : sb.inode_table_sects) * SECTOR_SIZE;

     uint8_t* buf = (uint8_t*)sys_malloc(buf_size);    // 申请的内存由堆内存管理模块清0后返回

     /** 2. 将"块"位图初始化并写入sb.block_bitmap_lba **/
     // 初始化块位图block_bitmap
     buf[0] |= 0x01;    // 第0个块预留给了根目录
     uint32_t block_bitmap_last_byte = block_bitmap_bit_len / 8;
     uint8_t block_bitmap_last_bit = block_bitmap_bit_len % 8;
     uint32_t last_size = SECTOR_SIZE - (block_bitmap_last_byte % SECTOR_SIZE);    // last_size是块位图所在的最后一个扇区中,不足一扇区的其余“无效”部分  (以字节为大小)

     // (1) 先将位图最后一字节到其所在扇区的结束位置，全置为1, 即超出实际块数的部分直接标为已占用, 将来就不会分配这些位对应的资源了(这些位是多余的, 无效的, 分配将出错！)
     memset(&buf[block_bitmap_last_byte], 0xff, last_size);
     // (2) 再将上一步中覆盖的最后一字节内的有效位重置为0
     uint8_t bit_idx = 0;
    while (bit_idx <= block_bitmap_last_bit){
        buf[block_bitmap_last_byte] &= ~(1 << bit_idx);
        bit_idx++;
    }
     // (3) 将位图写入磁盘
    ide_write(hd, sb.block_bitmap_lba, buf, sb.block_bitmap_sects);

    /** 3. 将inode位图初始化并写入sb.inode_bitmap_lba **/
    // 先清空缓冲区
    memset(buf, 0, buf_size);
    buf[0] |= 0x1;   // 第0个inode已分给根目录
    // 直接写入硬盘的inode位图位置
    ide_write(hd, sb.inode_bitmap_lba, buf, sb.inode_bitmap_sects);

    /** 4.将inode数组初始化并写入sb.inode_table_lba **/
    // 先清空缓冲区
    memset(buf, 0, buf_size);
    // 初始化第0个inode, 即根目录的信息
    struct inode* i = (struct inode*)buf;
    i->i_size = sb.dir_entry_size * 2;    // .和..目录项大小之和
    i->i_no = 0;
    i->i_blocks[0] = sb.data_start_lba;   // 由于上面的memset,i_blocks数组的其它元素都初始化为0
    // 将Inode数组写入硬盘
    ide_write(hd, sb.inode_table_lba, buf, sb.inode_table_sects);

    /** 5. 将根目录写入sb.data_start_lba **/
    // 写入根目录的内容, 即两个目录项.和..
    memset(buf, 0, buf_size);
    struct dir_entry* p_de = (struct dir_entry*) buf;

    memcpy(p_de->filename, ".", 1);       // 初始化当前目录“.”
    p_de->i_no = 0;
    p_de->f_type = FT_DIRECTORY;
    p_de++;

    memcpy(p_de->filename, "..", 2);     // 初始化当前目录的父目录
    p_de->i_no = 0;                      // 根目录的父目录依然是自己
    p_de->f_type = FT_DIRECTORY;

    // 将根目录写入磁盘
    ide_write(hd, sb.data_start_lba, buf, 1);
    printk("   root_dir_lba:0x%x\n", sb.data_start_lba);
    printk("%s format done\n", part->name);

    // 释放掉申请的内存缓冲区
    sys_free(buf);
}

/* 解析最上层路径, 返回剩余路径 */
char* path_parse(char* pathname, char* name_store) {
    if (pathname[0] == '/') {   // 根目录不需要单独解析
        /* 路径中出现1个或多个连续的字符'/',将这些'/'跳过,如"///a/b" */
        while(*(++pathname) == '/');
    }

    /* 开始一般的路径解析 */
    while (*pathname != '/' && *pathname != 0) {
        *name_store = *pathname;
        name_store++;
        pathname++;
    }
    if (pathname[0] == 0) {   // 若路径字符串为空则返回NULL
        return NULL;
    }
    return pathname;
}

/* 返回路径深度,比如/a/b/c,深度为3 */
int32_t path_depth_cnt(char* pathname) {
    ASSERT(pathname != NULL);
    char* p = pathname;
    char name[MAX_FILE_NAME_LEN];       // 用于path_parse的参数做路径解析
    uint32_t depth = 0;

    /* 解析路径,从中拆分出各级名称 */
    p = path_parse(p, name);
    while (name[0]) {
        depth++;
        memset(name, 0, MAX_FILE_NAME_LEN);
        if (p) {	     // 如果p不等于NULL,继续分析路径
            p  = path_parse(p, name);
        }
    }
    return depth;
}

/* 搜索文件pathname, 若找到则返回其inode号, 否则返回-1 */
static int search_file(const char* pathname, struct path_search_record* searched_record){
    // 若待查找的是根目录, 为避免下面无用的查找, 直接返回已知根目录信息
    if(!strcmp(pathname, "/") || !strcmp(pathname, "/.") || !strcmp(pathname, "/..")) {
        searched_record->parent_dir = &root_dir;
        searched_record->file_type = FT_DIRECTORY;
        searched_record->searched_path[0] = 0;  // 搜索路径置空
        return 0;
    }
    // 保证参数pathname至少的以'/'开头, 且长度符合规范的路径
    uint32_t path_len = strlen(pathname);
    ASSERT(pathname[0] == '/' && path_len > 1 && path_len < MAX_PATH_LEN);

    char* sub_path = (char*)pathname;
    struct dir* parent_dir = &root_dir;  // 要从根目录开始往下查找文件
    struct dir_entry dir_e;
    // 定义name数组用来记录路径解析出来的各级名称, 如路径"/a/b/c", 数组name每次的值分别是'a', 'b', 'c'
    char name[MAX_FILE_NAME_LEN] = {0};

    searched_record->parent_dir = parent_dir;
    searched_record->file_type = FT_UNKNOWN;
    uint32_t parent_inode_no = 0;            // 父目录的inode号(从根目录开始)
    sub_path = path_parse(sub_path, name);   // 此时sub_path已剥去了上层路径成了子路径, 当前路径保存在name(即我们要查找的文件)

    while (name[0]){    // 若第一个字符就是结束符, 结束循环
        ASSERT(strlen(searched_record->searched_path) < 512);  // 记录查找过的路径, 但不能超过searched_path的长度512字节

        // 将每次解析过的路径追加到searched_record中
        strcat(searched_record->searched_path, "/");
        strcat(searched_record->searched_path, name);

        // 在所给的目录中查找文件
        if(search_dir_entry(cur_part, parent_dir, name, &dir_e)) {
            memset(name, 0, MAX_FILE_NAME_LEN); // 清零，便于下次查找
            if(sub_path){  // 如果sub_path不为NULL, 还可继续拆分路径
                sub_path = path_parse(sub_path, name);
            }
            if(FT_DIRECTORY == dir_e.f_type) {  // 判断上层路径name是否仍是目录
                parent_inode_no = parent_dir->inode->i_no;
                // 打开的目录记得关闭, 否则会造成内存泄漏
                dir_close(parent_dir);
                // 更新父目录
                parent_dir = dir_open(cur_part, dir_e.i_no);
                searched_record->parent_dir = parent_dir;
                continue;
            } else if (FT_REGULAR == dir_e.f_type) {  // 若上层路径name是普通文件
                searched_record->file_type = FT_REGULAR;
                return dir_e.i_no;
            }
        } else {  // 若找不到名为name的文件则返回-1
            /** 注意：找不到目录项时要留着parent_dir不要关闭, 若主调函数要创建新文件的话需要在该目录中创建 **/
            return -1;
        }
    }
    // 至此, 必然是遍历了所有完整的路径并且最后一层路径不是普通文件, 而是目录
    dir_close(searched_record->parent_dir);
    /* 保存“被查找目录”的直接父目录 */
    searched_record->parent_dir = dir_open(cur_part, parent_inode_no);
    searched_record->file_type = FT_DIRECTORY;
    return dir_e.i_no;
}

/* 打开或者创建文件成功之后, 返回文件描述符, 否则返回-1 */
int32_t sys_open(const char* pathname, uint8_t flags) {
    // 对目录要用dir_open, 这里只有open文件
    if(pathname[strlen(pathname) - 1] == '/') {
        printk("can`t open a directory %s\n",pathname);
        return -1;
    }
    ASSERT(flags <= 7);  // 限制flag在 O_RDONLY|O_WRONLY|O_RDWR|O_CREAT 之内
    int32_t fd = -1;    // 默认为找不到文件

    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record)); // 生成路径搜索记录变量用来记录文件查找时所遍历过的目录
    // 记录目录深度, 帮助判断中间某个目录不存在的情况
    uint32_t pathname_depth = path_depth_cnt((char*)pathname);
    int inode_no = search_file(pathname, &searched_record);  // 先检查文件是否存在
    bool found = inode_no != -1? true : false;
    if(searched_record.file_type == FT_DIRECTORY){
        printk("can`t open a direcotry with open(), use opendir() to instead\n");
        dir_close(searched_record.parent_dir);
        return -1;
    }
    // 获取已搜索路径的深度并判断是否中途就断掉了
    uint32_t path_searched_depth = path_depth_cnt(searched_record.searched_path);
    if(pathname_depth != path_searched_depth) {  // 先判断是否把pathname的各层目录都访问到了, 即是否在某个中间目录就失败了？
        printk("cannot access %s: Not a directory, subpath %s is`t exist\n", pathname, searched_record.searched_path);
        dir_close(searched_record.parent_dir);
        return -1;
    }
    // 至此, 说明确实完整的遍历完了pathname指定的路径
    if(!found && !(flags & O_CREAT)) { // 若是在最后一个路径上没找到, 并且不是要创建文件的话, 就返回-1
        printk("in path %s, file %s is`t exist\n", searched_record.searched_path, (strrchr(searched_record.searched_path, '/') + 1));
        dir_close(searched_record.parent_dir);
        return -1;
    } else if(found && (flags & O_CREAT)){  // 若是要创建的文件已存在
        printk("%s has already exist!\n", pathname);
        dir_close(searched_record.parent_dir);
        return -1;
    }

    switch (flags & O_CREAT) {
        case O_CREAT:
            printk("creating file\n");
            fd = file_create(searched_record.parent_dir, (strrchr(pathname, '/') + 1), flags);
			dir_close(searched_record.parent_dir);
	 		break;
        default:
            // 其余情况则是：打开已存在文件, O_RDONLY, O_WRONLY, O_RDWR
            fd = file_open(inode_no, flags);
    }
    return fd; // 返回文件描述符
}

/* 将文件描述符转化为文件表的下标 */
uint32_t fd_local2global(uint32_t local_fd){
    struct task_struct* cur = running_thread();
    int32_t global_fd = cur->fd_table[local_fd];
    ASSERT(global_fd >= 0 && global_fd < MAX_FILE_OPEN);
    return (uint32_t)global_fd;
}

/* 关闭文件描述符fd指向的文件, 成功返回0，否则返回-1 */
int32_t sys_close(int32_t fd){
    int32_t ret = -1;    // 返回值默认为-1, 即失败
    if(fd > 2) {
        uint32_t global_fd = fd_local2global(fd);    // 获取文件描述符对应的全局文件表中的下标
        if (is_pipe(fd)) {
            // 判断一下此管道上的描述符全部都被关闭,若是,则释放管道的环形缓冲区
            if (--file_table[global_fd].fd_pos == 0) {
                mfree_page(PF_KERNEL, file_table[global_fd].fd_inode, 1);
                file_table[global_fd].fd_inode = NULL;
            }
            ret = 0;
      } else {
            ret = file_close(&file_table[global_fd]);
      }
        running_thread()->fd_table[fd] = -1;      // 使该文件描述符位在下次可再次分配
    }
    return ret;
}

/* 将buf中连续count个字节写入文件描述符fd, 成功则返回写入的字节数, 失败则返回-1 */
int32_t sys_write(int32_t fd, const void* buf, uint32_t count) {
    if(fd < 0){
        printk("sys_write: fd error\n");
        return -1;
    }
    if(fd == stdout_no) {
        if(is_pipe(fd)){    // 如果标准输出是管道(说明标准输出被重定向为管道缓冲区了)
            return pipe_write(fd, buf, count);
        }
        char tmp_buf[1024] = {0};
        memcpy(tmp_buf, buf, count);
        console_put_str(tmp_buf);
        return count;
    } else if(is_pipe(fd)){
        // 若fd指向的是管道, 则特定地调用管道的方法
        return pipe_write(fd, buf, count);
    } else {
        // 其他情况下, sys_write都是往文件中写数据
        uint32_t fd_idx = fd_local2global(fd);
        struct file* wr_file = &file_table[fd_idx];
        if(wr_file->fd_flag & O_WRONLY || wr_file->fd_flag & O_RDWR) {
            uint32_t bytes_written = file_write(wr_file, buf, count);
            return bytes_written;
        } else {
            console_put_str("sys_write: not allowed to write file without flag O_RDWR or O_WRONLY\n");
            return -1;
        }
    }
}

/* 从文件描述符fd指向的文件中读取count个字节到buf, 成功则返回读入的字节数, 失败返回-1 */
int32_t sys_read(int32_t fd, void* buf, uint32_t count) {
    ASSERT(buf != NULL);
    int32_t ret = -1;    // 默认返回值为-1
    uint32_t global_fd = 0;
    if(fd < 0 || fd == stdout_no || fd == stderr_no){
        printk("sys_read: fd error\n");
    } else if (fd == stdin_no) {    // 若是读取输入设备
        if(is_pipe(fd)) {
            // 如果标准输入被重定向为管道缓冲区
            ret = pipe_read(fd, buf, count);
        } else {
            char* buffer = buf;
            uint32_t bytes_read = 0;
            while (bytes_read < count) {
                *buffer = ioq_getchar(&kbd_buf); // 每次从kbd_buf(存储键盘输入的环形缓冲区)中获取一个字符, 存入buf中
                bytes_read++;
                buffer++;
            }
            ret = (bytes_read == 0? -1 : (int32_t)bytes_read);
        }
    } else if(is_pipe(fd)){
        // 若fd对应的是管道, 则特定地使用管道的读取方法
        ret = pipe_read(fd, buf, count);
    } else {
        global_fd = fd_local2global(fd);
        ret = file_read(&file_table[global_fd], buf, count);
    }
    return ret;
}

/* 重置用于文件读写操作的偏移指针, 成功时返回新的偏移量, 出错时返回-1 */
int32_t sys_lseek(int32_t fd, int32_t offset, uint8_t whence) {
    if(fd < 0) {
        printk("sys_lseek: fd error\n");
        return -1;
    }
    ASSERT(whence > 0 && whence < 4);    // whence取值只有三种: 1 SEEK_SET, 2 SEEK_CUR, 3 SEEK_END
    uint32_t global_fd = fd_local2global(fd);
    struct file* pf = &file_table[global_fd];
    int32_t new_pos = 0;    // 新的文件读写偏移量必须位于文件大小之内
    int32_t file_size = (int32_t)pf->fd_inode->i_size;

    switch (whence) {
        // 新的读写位置是相对于文件开头再增加offset个位移量
        case SEEK_SET:
            new_pos = offset;
            break;
        // 新的读写位置是相对于当前的位置增加offset个位移量
        case SEEK_CUR:
            new_pos = (int32_t)pf->fd_pos + offset;
            break;
        // 新的读写位置是相对于文件尺寸再增加offset个位移量, 此时的offset应为负值
        case SEEK_END:
            new_pos = file_size + offset;
    }
    if(new_pos < 0 || new_pos > (file_size - 1)){
        printk("sys_lseek: new_pos position is invalid\n");
        return -1;
    }
    pf->fd_pos = new_pos;
    return pf->fd_pos;
}

/* 删除文件(非目录), 成功返回0, 失败返回-1 */
int32_t sys_unlink(const char* pathname) {
    ASSERT(strlen(pathname) < MAX_PATH_LEN);
    // 先检查待删除的文件是否存在
    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record));
    int inode_no = search_file(pathname, &searched_record);
    ASSERT(inode_no != 0);
    if (inode_no == -1) {
        printk("file %s not found!\n", pathname);
        dir_close(searched_record.parent_dir);
        return -1;
    }
    if(searched_record.file_type == FT_DIRECTORY){
        printk("can`t delete a direcotry with unlink(), use rmdir() to instead\n");
        dir_close(searched_record.parent_dir);
        return -1;
    }
    // 检查要删除的文件是否已在“文件打开表”中
    uint32_t file_idx = 0;
    while (file_idx < MAX_FILE_OPEN) {
        if(file_table[file_idx].fd_inode != NULL && (uint32_t)inode_no == file_table[file_idx].fd_inode->i_no){
            break;
        }
        file_idx++;
    }
    if( file_idx < MAX_FILE_OPEN){  // 若是file_idx小于MAX_FILE_OPEN说明上一个循环未遍历完就提前跳出了,即要删除的文件在全局文件表(文件打开表)中
        dir_close(searched_record.parent_dir);
        printk("file %s is in use, not allow to delete!\n", pathname);
        return -1;
    }
    ASSERT(file_idx == MAX_FILE_OPEN);

    // 至此, 说明文件确确实实可以被删除了
    // 为delete_dir_entry申请缓冲区
    void* io_buf = sys_malloc(SECTOR_SIZE * 2);
    if(io_buf == NULL){
        dir_close(searched_record.parent_dir);
        printk("sys_unlink: malloc for io_buf failed\n");
        return -1;
    }
    struct dir* parent_dir = searched_record.parent_dir;
    delete_dir_entry(cur_part, parent_dir, inode_no, io_buf);
    inode_release(cur_part, inode_no);

    sys_free(io_buf);
    dir_close(searched_record.parent_dir);
    return 0;  // 成功删除文件, 返回0
}

/* 创建目录pathname, 成功返回0, 失败则返回-1 */
int32_t sys_mkdir(const char* pathname) {
    uint8_t rollback_step = 0;                  // 用于操作失败时回滚各资源状态
    void* io_buf = sys_malloc(SECTOR_SIZE * 2); // 提前申请两扇区大小的内存缓冲区
    if (io_buf == NULL) {
        printk("sys_mkdir: sys_malloc for io_buf failed\n");
        return -1;
    }
    /** Step 1: 提前确认当前目录下是否有同名目录或者文件 **/
    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record));
    int inode_no = search_file(pathname, &searched_record);
    if(inode_no != -1) {  // 如果找到了同名目录或文件, 失败返回
        printk("sys_mkdir: file or directory %s exist!\n", pathname);
        rollback_step = 1;
        goto rollback;
    } else {    // 若未找到, 也要判断是在最终目录没找到还是某个中间目录就不存在
        uint32_t pathname_depth = path_depth_cnt((char*)pathname);
        uint32_t path_searched_depth = path_depth_cnt(searched_record.searched_path);
        // 先判断是否把pathname各层都访问到了, 即是否在某个中间目录就失败了
        if(pathname_depth != path_searched_depth) {
            printk("sys_mkdir: can`t access %s, subpath %s is`t exist\n", pathname, searched_record.searched_path);
            rollback_step = 1;
            goto rollback;
        }
    }

    /** Step 2: 为新目录创建inode **/
    struct dir* parent_dir = searched_record.parent_dir;  // 指向”被创建目录“的父目录
    char* dirname = strrchr(searched_record.searched_path, '/') + 1; // 从后往前获取字符'/'第一次出现的地址, 如a/b/c, dirname将等于c
    // 为要创建的目录文件分配一个inode编号
    inode_no = inode_bitmap_alloc(cur_part);
    if (inode_no == -1){
        printk("sys_mkdir: allocate inode failed\n");
        rollback_step = 1;
        goto rollback;
    }

    struct inode new_dir_inode;
    inode_init(inode_no, &new_dir_inode);  // 初始化目录文件的inode

    /** Step 3: 为该目录文件分配一个块，用来写入目录.和.. **/
    uint32_t block_bitmap_idx = 0;     // 用来记录block对应于block_bitmap中的索引
    int32_t block_lba = block_bitmap_alloc(cur_part);
    if (block_lba == -1) {
        printk("sys_mkdir: block_bitmap_alloc for create directory failed\n");
        rollback_step = 2;
        goto rollback;
    }
    // 老规矩：没分配一个块就要将位图同步到硬盘
    block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
    ASSERT(block_bitmap_idx != 0);
    bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

    new_dir_inode.i_blocks[0] = block_lba;

    // 将当前目录的目录项'.'和'..'写入目录文件中
    memset(io_buf, 0, SECTOR_SIZE * 2);
    struct dir_entry* p_de = (struct  dir_entry*)io_buf;
    memcpy(p_de->filename, ".", 1);
    p_de->i_no = inode_no;
    p_de->f_type = FT_DIRECTORY;

    p_de++;
    memcpy(p_de->filename, "..", 2);
    p_de->i_no = parent_dir->inode->i_no;
    p_de->f_type = FT_DIRECTORY;
    ide_write(cur_part->my_disk, new_dir_inode.i_blocks[0], io_buf, 1);

    new_dir_inode.i_size = 2 * cur_part->sb->dir_entry_size;

    /** Step 4: 在新目录文件的父目录中添加新目录文件的目录项 **/
    struct dir_entry new_dir_entry;
    memset(&new_dir_entry, 0, sizeof(struct dir_entry));
    create_dir_entry(dirname, inode_no, FT_DIRECTORY, &new_dir_entry);  // 初始化目录项new_dir_entry

    memset(io_buf, 0, SECTOR_SIZE * 2);
    if(!sync_dir_entry(parent_dir, &new_dir_entry, io_buf)){
        printk("sys_mkdir: sync_dir_entry to disk failed!\n");
        rollback_step = 2;
        goto rollback;
    }
    /** Step 5: 将以上资源的变更同步到硬盘中 **/
    // 同步父目录inode 和 新目录inode到硬盘
    memset(io_buf, 0, SECTOR_SIZE * 2);
    inode_sync(cur_part, parent_dir->inode, io_buf);

    memset(io_buf, 0, SECTOR_SIZE * 2);
    inode_sync(cur_part, &new_dir_inode, io_buf);
    sys_free(io_buf);
    // 将inode位图同步到硬盘
    bitmap_sync(cur_part, inode_no, INODE_BITMAP);
    // 关闭所创建目录的父目录
    dir_close(searched_record.parent_dir);
    return 0;

    rollback:
    switch (rollback_step) {
        case 2:
            // 如果目录文件的block分配失败, 之前inode位图中分配的inode_no要回收
            bitmap_set(&cur_part->inode_bitmap, inode_no, 0);
        case 1:
            // 关闭所创建的目录的父目录
            dir_close(searched_record.parent_dir);
            break;
    }
    sys_free(io_buf);
    return -1;
}

/* 目录打开成功后返回目录指针, 失败返回NULL */
struct dir* sys_opendir(const char* name) {
    ASSERT(strlen(name) < MAX_PATH_LEN);
    // 如果是根目录'/' 直接返回&root_dir
    if (name[0] == '/' && (name[1] == 0 || name[1] == '.')){
        return &root_dir;
    }

    // 先检查待打开的目录是否存在
    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record));
    int inode_no = search_file(name, &searched_record);
    struct dir* ret = NULL;
    if (inode_no == -1) {    // 若要查找的文件不存在, 输出错误信息
        printk("In %s, sub path %s not exist\n", name, searched_record.searched_path);
    } else {    // 名为name的文件存在,进一步判断是否为目录类型
        if (searched_record.file_type == FT_REGULAR) {
            printk("%s is regular file!\n", name);
        } else if (searched_record.file_type == FT_DIRECTORY){
            ret = dir_open(cur_part, inode_no);
        }
    }
    dir_close(searched_record.parent_dir);
    return ret;
}

/* 成功关闭目录dir返回0, 失败则返回-1 */
int32_t sys_closedir(struct dir* dir) {
    int32_t ret = -1;
    if(dir != NULL) {
        dir_close(dir);
        ret = 0;
    }
    return ret;
}

/* 读取目录dir的一个目录项, 成功后返回其目录项地址, 到目录尾时或出错时返回NULL */
struct dir_entry* sys_readdir(struct dir* dir){
    ASSERT(dir != NULL);
    return dir_read(dir);
}

/* 把目录dir的指针dir_pos重置为0 */
void sys_rewinddir(struct dir* dir) {
    dir->dir_pos = 0;
}

/* 删除空目录, 成功时返回0, 失败时返回-1 */
int32_t sys_rmdir(const char* pathname) {
    // 先检查待删除的文件是否存在
    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record));
    int inode_no = search_file(pathname, &searched_record);
    ASSERT(inode_no != 0);

    int retVal = -1;  // 默认返回值
    if (inode_no == -1){  // 说明未找到该目录
        printk("In %s, sub path %s not exist\n", pathname, searched_record.searched_path);
    } else { // 找到了pathname文件, 接下来判断该文件是目录还是普通文件
        if(searched_record.file_type == FT_REGULAR){
            printk("%s is regular file!\n", pathname);
        } else {
            // 进一步判断该目录是否为空
            struct dir* dir = dir_open(cur_part, inode_no);
            if (!dir_is_empty(dir)){   // 非空目录不可删除
                printk("dir %s is not empty, it is not allowed to delete a nonempty directory!\n", pathname);
            } else {
                // 当前目录为空目录, 尝试删除
                if (!dir_remove(searched_record.parent_dir, dir)){
                    retVal = 0;
                }
            }
            dir_close(dir);
        }
    }
    dir_close(searched_record.parent_dir);
    return retVal;
}

/* 实现sys_getcwd的基础: 获得当前目录的父目录的inode编号 */
static uint32_t get_parent_dir_inode_nr(uint32_t child_inode_nr, void* io_buf) {
    // 获得当前目录的inode
    struct inode* child_dir_inode = inode_open(cur_part, child_inode_nr);
    // 当前目录的目录项".."里含有父目录的inode编号, 位于当前目录的第0块
    uint32_t block_lba = child_dir_inode->i_blocks[0];
    ASSERT(block_lba >= cur_part->sb->data_start_lba);
    inode_close(child_dir_inode);  // child_dir_node利用完毕, 记得关闭

    ide_read(cur_part->my_disk, block_lba, io_buf, 1);  // 将块的内容读出到io_buf
    struct dir_entry* dir_e = (struct dir_entry*)io_buf;
    // 第一个目录项是“.” 第二个是 “..”
    ASSERT(dir_e[1].i_no < 4096 && dir_e[1].f_type == FT_DIRECTORY);
    return dir_e[1].i_no;    // 返回..的inode编号
}

/* 实现sys_getcwd的基础: 在inode编号为p_inode_nr的目录中查找inode编号为c_inode_nr的子目录的名字, 并将名字存入缓冲区path, 成功返回0, 失败返回-1 */
static int get_child_dir_name(uint32_t p_inode_nr, uint32_t c_inode_nr, char* path, void* io_buf) {
    struct inode* parent_dir_inode = inode_open(cur_part, p_inode_nr);
    // 填充all_blocks, 将父目录的所占扇区地址全部写入all_blocks
    uint32_t all_blocks[140] = {0}, block_cnt = 12;
    uint8_t block_idx = 0;
    while (block_idx < 12){
        all_blocks[block_idx] = parent_dir_inode->i_blocks[block_idx];
        block_idx++;
    }
    if(parent_dir_inode->i_blocks[12] != 0){
        ide_read(cur_part->my_disk, parent_dir_inode->i_blocks[12], all_blocks + 12, 1);
        block_cnt = 140;
    }
    inode_close(parent_dir_inode);    // 利用完父目录节点记得关闭

    // 要遍历所有目录项, 找到c_inode_nr对应的目录项, 并将名字追加到path中
    struct dir_entry* dir_e = (struct dir_entry*)io_buf;
    uint32_t dir_entry_size = cur_part->sb->dir_entry_size;
    uint32_t dir_entrys_per_sec = (512 / dir_entry_size);
    // 遍历每个块
    block_idx = 0;
    while (block_idx < block_cnt) {
        if(all_blocks[block_idx] != 0) {
            ide_read(cur_part->my_disk, all_blocks[block_idx], io_buf, 1);
            // 遍历每个目录项
            uint8_t dir_e_idx = 0;
            while (dir_e_idx < dir_entrys_per_sec) {
                if((dir_e + dir_e_idx)->i_no == c_inode_nr) {
                    // 追加目录名字到path中
                    strcat(path, "/");
                    strcat(path, (dir_e + dir_e_idx)->filename);
                    return 0;
                }
                dir_e_idx++;
            }
        }
        block_idx++;
    }
    return -1;
}

/* 实现sys_getcwd: 获取当前工作路径写入buf中, buf: 由用户或者操作系统提供， size为buf的大小*/
char* sys_getcwd(char* buf, uint32_t size) {
    ASSERT(buf != NULL);
    void* io_buf = sys_malloc(SECTOR_SIZE);
    if(io_buf == NULL) {
        return NULL;
    }
    // 获取任务的当前工作目录的inode编号（存储在pcb中）
    struct task_struct* cur_thread = running_thread();
    int32_t child_inode_nr = cur_thread->cwd_inode_nr;
    int32_t parent_inode_nr = 0;
    ASSERT(child_inode_nr >= 0 && child_inode_nr < 4096);    // 最大支持4096个inode
    // 若当前目录是根目录, 直接返回‘/’
    if (child_inode_nr == 0) {
        buf[0] = '/';
        buf[1] = 0;
        return buf;
    }
    // 定义full_path_reverse[MAX_PATH_LEN], 它用于存储工作目录所在的全路径, 即绝对路径
    memset(buf, 0, size);
    char full_path_reverse[MAX_PATH_LEN] = {0};
    // 从下往上逐层找父目录, 直到找到根目录为止, 当child_inode_nr为根目录的inode编号时停止查找
    while (child_inode_nr != 0){
        parent_inode_nr = get_parent_dir_inode_nr(child_inode_nr, io_buf);
        if(get_child_dir_name(parent_inode_nr, child_inode_nr, full_path_reverse, io_buf) == -1){
            // 在父目录下未找到当前目录的名字, 失败退出
            sys_free(io_buf);
            return NULL;
        }
        child_inode_nr = parent_inode_nr;  // 更新"当前目录"
    }
    ASSERT(strlen(full_path_reverse) <= size);  // 全路径名不超过buf的大小
    // full_path_reverse数组中的路径是反着的, 即子目录在前, 父目录在后, 需要反转
    char* last_slash;  // 表示最后一个斜杠地址
    while (last_slash = strrchr(full_path_reverse, '/')) {
        uint16_t len = strlen(buf);
        strcpy(buf + len, last_slash);
        /* 在full_path_reverse中添加结束字符,做为下一次执行strcpy中last_slash的边界 */
        *last_slash = 0;
    }
    sys_free(io_buf);
    return buf;
}

/* 更改当前工作目录为绝对路径path, 成功则返回0, 失败返回-1 */
int32_t sys_chdir(const char* path) {
    int32_t ret = -1;    // 默认返回值
    // 先保证path要存在
    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record));
    int inode_no = search_file(path, &searched_record);
    if (inode_no != -1) {
        if (searched_record.file_type == FT_DIRECTORY) {
            // 更改任务的当前工作目录
            running_thread()->cwd_inode_nr = inode_no;
            ret = 0;
        } else {
            printk("sys_chdir: %s is regular file or other!\n", path);
        }
    }
    // search_file会打开path的父目录, 记得关闭
    dir_close(searched_record.parent_dir);
    return ret;
}

/* 在参数buf中填充文件结构相关信息, 成功时返回0, 失败时返回-1 */
int32_t sys_stat(const char* path, struct stat* buf) {
    // 若直接查看根目录
    if(!strcmp(path, "/") || !strcmp(path, "/.") || !strcmp(path, "/..")) {
        buf->st_filetype = FT_DIRECTORY;
        buf->st_ino = 0;
        buf->st_size = root_dir.inode->i_size;
        return 0;
    }
    int32_t ret = -1;    // 默认返回值
    // 开始搜索path指向的文件
    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record));
    int inode_no = search_file(path, &searched_record);
    if (inode_no != -1) {
        struct inode* obj_inode = inode_open(cur_part, inode_no);   // 只为获得文件大小
        buf->st_size = obj_inode->i_size;
        inode_close(obj_inode);
        buf->st_filetype = searched_record.file_type;
        buf->st_ino = inode_no;
        ret = 0;
    } else {
        printk("sys_stat: %s not found\n", path);
    }
    dir_close(searched_record.parent_dir);
    return ret;
}

/* 向屏幕输出一个字符 */
void sys_putchar(char char_asci) {
   console_put_char(char_asci);
}

/* 显示系统支持的内部命令 */
void sys_help(void) {
    printk("buildin commands:\n ls: show directory or file information\n cd: change current work directory\n mkdir: create a directory\n rmdir: remove a empty directory\n rm: remove a regular file\n pwd: show current work directory\n ps: show process information\n clear: clear screen\n shortcut key:\n ctrl+l: clear screen\n ctrl+u: clear input\n");
}

/* 文件系统初始化函数：在磁盘上搜索文件系统, 若没有则格式化分区创建文件系统 */
void filesys_init(){
    uint8_t channel_no = 0, dev_no = 0, part_idx = 0;

    /* sb_buf用来存储从硬盘上读入的超级块 */
    struct super_block* sb_buf = (struct super_block*)sys_malloc(SECTOR_SIZE);

    if(sb_buf == NULL) {
        PANIC("alloc memory failed!");
    }
    printk("searching filesystem......\n");

    // 遍历ide通道
    while (channel_no < channel_cnt) {
        // 一个通道顶多只有2个磁盘(一主一从)
        dev_no = 0;
        while (dev_no < 2){
            if(dev_no == 0) {    // 跨过裸盘hd60M.img
                dev_no++;
                continue;
            }
            // 从盘hd80M.img(本项目的文件系统就在这里初始化)
            struct disk* hd = &channels[channel_no].devices[dev_no];
            struct partition* part = hd->prim_parts;    // 指向磁盘的主分区数组起始地址
            while (part_idx < 12) {    // 4个主分区 + 8个逻辑分区
                if (part_idx == 4) {  // 开始处理逻辑分区
	       part = hd->logic_parts;
	    	}

                // channels数组是全局变量, 默认值为0, disk属于其嵌套结构, partition又为disk的嵌套结构,partition中的成员默认也为0
                // 若partition未初始化, 则partition中的成员仍为0
                if(part->sec_cnt != 0) {    // 如果分区存在
                    memset(sb_buf, 0, SECTOR_SIZE);
                    // 读出分区的超级块, 根据魔数是否正确来判断是否存在文件系统
                    ide_read(hd, part->start_lba + 1, sb_buf, 1);
                    if(sb_buf->magic == 0x19980924) {
                        printk("%s has filesystem\n", part->name);
                    }else{    // 不支持其他文件系统, 一律按无文件系统处理
                        printk("formatting %s`s partition %s......\n", hd->name, part->name);
                        partition_format(part);
                    }
                }
                part_idx++;
                part++;      // 扫描下一分区
            }
            dev_no++;    // 扫描下一磁盘
        }
        channel_no++;    // 扫描下一通道
    }
    sys_free(sb_buf);

    // 确定默认操作的分区
    char default_part[8] = "sdb1";
    // 挂载分区(本系统实现只是指定要操作的分区为default_part)
    list_traversal(&partition_list, mount_partition, (int)default_part);

    // 将当前分区的根目录打开
    open_root_dir(cur_part);
    // 初始化文件表
    uint32_t fd_idx = 0;
    while (fd_idx < MAX_FILE_OPEN){
        file_table[fd_idx++].fd_inode = NULL;
    }
}