#include "ide.h"
#include "sync.h"
#include "io.h"
#include "stdio.h"
#include "stdio-kernel.h"
#include "interrupt.h"
#include "memory.h"
#include "debug.h"
#include "console.h"
#include "timer.h"
#include "string.h"
#include "list.h"

/* 定义宏 用于表示硬盘各寄存器的端口号 */
#define reg_data(channel)	     (channel->port_base + 0)
#define reg_error(channel)	     (channel->port_base + 1)
#define reg_sect_cnt(channel)    (channel->port_base + 2)
#define reg_lba_l(channel)	     (channel->port_base + 3)
#define reg_lba_m(channel)	     (channel->port_base + 4)
#define reg_lba_h(channel)	     (channel->port_base + 5)
#define reg_dev(channel)	     (channel->port_base + 6)
#define reg_status(channel)	     (channel->port_base + 7)
#define reg_cmd(channel)	     (reg_status(channel))
#define reg_alt_status(channel)  (channel->port_base + 0x206)
#define reg_ctl(channel)	     reg_alt_status(channel)

/* reg_status寄存器的一些关键位 */
#define BIT_STAT_BSY     0x80   // 硬盘忙
#define BIT_STAT_DRDY    0x40   // 设备就绪, 等待指令
#define BIT_STAT_DRQ     0x8    // 设备已准备好数据, 随时可以输出

/* device寄存器的一些关键位 */
#define BIT_DEV_MBS 0xa0        // 第7位和第5位固定为1
#define BIT_DEV_LBA 0x40        // 表示启用LBA方式寻址
#define BIT_DEV_DEV 0x10        // 主盘或从盘（1代表从盘）

/* 一些硬盘操作命令, 在这里只定义三个先 */
#define CMD_IDENTIFY     0xec    // identify指令：硬盘识别
#define CMD_READ_SECTOR  0x20    // 读扇区指令
#define CMD_WRITE_SECTOR 0x30    // 写扇区指令

/* 定义可读写的最大扇区数，用于调试 */
#define max_lba ((80 * 1024 * 1024 / 512) - 1)    // 因为我们的硬盘是80M的

uint8_t channel_cnt;            // 表示机器上的ata通道数, 这里比较随便，仅是通过硬盘数反推有几个通道数
struct ide_channel channels[2]; // 表示有两个ide通道

int32_t ext_lba_base = 0;       // 用于记录总拓展分区的起始lba, 初始为0, partition_scan时以此为标记
uint8_t p_no = 0, l_no = 0;     // 用来记录硬盘主分区和逻辑分区的下标
struct list partition_list;     // 分区队列

/* 构建1个16字节大小的结构体, 用于存分区表项 */
struct partition_table_entry {
    uint8_t bootable;    // 是否可引导(其实就是活动分区标记)
    uint8_t start_head;  // 分区起始磁头号
    uint8_t start_sec;   // 分区起始扇区号
    uint8_t start_chs;   // 分区起始柱面号
    uint8_t fs_type;     // 文件系统类型ID
    uint8_t end_head;    // 分区结束磁头号
    uint8_t end_sec;     // 分区结束扇区号
    uint8_t end_chs;     // 分区结束柱面号

    /* 最需要关注的两项 */
    uint32_t start_lba;  // 本分区起始扇区的lba地址
    uint32_t sec_cnt;    // 分区容量扇区数
}__attribute__((packed)); // 保证此结构是16字节大小

/* 引导扇区, mbr或ebr所在的扇区 */
struct boot_sector {
    uint8_t other[446];    // 引导代码
    struct partition_table_entry partition_table[4];   // 分区表有4个表项, 共16*4 = 64字节
    uint16_t signature;   // 启动扇区的结束标志是0x55,0xaa
} __attribute__((packed));

/* 选择读写的硬盘 */
static void select_disk(struct disk* hd){
    uint8_t reg_device = BIT_DEV_MBS | BIT_DEV_LBA; // 先初始化一下device寄存器的“部分”状态
    if(hd->dev_no == 1) {    // 如果是从盘就置DEV位为1
        reg_device |= BIT_DEV_DEV; // 形成“完整”状态
    }
    // 将该状态写入硬盘所在通道的device寄存器
    outb(reg_dev(hd->my_channel), reg_device);
}

/* 向硬盘控制器写入起始扇区地址以及要读写的扇区数 */
static void select_sector(struct disk* hd, uint32_t lba, uint8_t sec_cnt) {
    ASSERT(lba <= max_lba);
    struct ide_channel* channel = hd->my_channel;

    // 写入要读写的扇区数
    outb(reg_sect_cnt(channel), sec_cnt);  // 如果sec_cnt位0，则表示写入256个扇区
    // 写入扇区起始地址：lba地址(即扇区号)
    outb(reg_lba_l(channel), lba);       // lba地址的低8位, outb函数中的汇编指令outb %b0, %w1会只用al
    outb(reg_lba_m(channel), lba>>8);    // lba地址的8~15位
    outb(reg_lba_h(channel), lba>>16);   // lba地址的16~23位

    // 因为LBA的第24~27位写在device寄存器的低4位中, 故重新把device寄存器写一遍，保留原信息的同时往低4位补充
    outb(reg_dev(channel), BIT_DEV_MBS | BIT_DEV_LBA | (hd->dev_no == 1? BIT_DEV_DEV : 0) | lba >> 24);
}

/* 向通道发出命令cmd */
static void cmd_out(struct ide_channel* channel, uint8_t cmd) {
    channel->expecting_intr = true; // 只要向硬盘发出了命令, 就将该通道标记设置为true, 为硬盘中断处理程序埋下伏笔
                                    // 表示将来该通道发出的中断信号也许就是此次命令操作引起的, 因此该通道期待来自硬盘的中断
    outb(reg_cmd(channel), cmd);
}

/* 将硬盘的sec_cnt个扇区的数据读入到buf */
static void read_from_sector(struct disk* hd, void* buf, uint8_t sec_cnt) {
    uint32_t size_in_byte;
    if(sec_cnt == 0){  // sec_cnt为8位变量, 若赋值256给该参数(1|00000000)会丢掉高位1变成0
        size_in_byte = 256 * 512;
    }else{
        size_in_byte = sec_cnt * 512;
    }
    insw(reg_data(hd->my_channel), buf, size_in_byte / 2);
}

/* 将buf中的sec_cnt个扇区的数据写入到硬盘中 */
static void write2sector(struct disk* hd, void* buf, uint8_t sec_cnt) {
    uint32_t size_in_byte;
    if (sec_cnt == 0) {
        size_in_byte = 256 * 512;
    } else {
        size_in_byte = sec_cnt * 512;
    }
    outsw(reg_data(hd->my_channel), buf, size_in_byte / 2);
}

/* 驱动程序用于让出cou使用权, 等待硬盘30秒 */
static bool busy_wait(struct disk* hd) {
    struct ide_channel* channel = hd->my_channel;
    uint16_t time_limit = 30 * 1000;    // 等待30 000毫秒

    while (time_limit -= 10 >= 0){
        // 在30秒内不断读取status寄存器的BSY位, 若为1则表示硬盘还在工作, 让出cpu, 当前线程休眠10”毫秒“
        if((inb(reg_status(channel)) & BIT_STAT_BSY)){
           mtime_sleep(10);
        } else{    // 否则说明硬盘不忙, 即已经完成工作, 读取DRQ位的值，DRQ为1的话表示硬盘已准备好数据
            return (inb(reg_status(channel)) & BIT_STAT_DRQ);
        }
    }
    // 到这步的话, 说明30秒内硬盘都没完成任务, 直接返回失败
    return false;
}

/* 从硬盘读取sec_cnt个扇区到buf：对前面工具函数的封装 */
void ide_read(struct disk* hd, uint32_t lba, void* buf, uint32_t sec_cnt) {
    ASSERT(lba <= max_lba && sec_cnt > 0);
    lock_acquire(&hd->my_channel->lock);    // 先将硬盘所在的通道上锁，确保一次只操作该通道上的一块硬盘

    // 1. 选择操作的硬盘
    select_disk(hd); // 即是主盘还是从盘？要将状态写入到hd所属通道的device寄存器中

    uint32_t secs_op;        // 每次操作的扇区数
    uint32_t secs_done = 0;  // 已完成的扇区数
    while (secs_done < sec_cnt) {   // 循环读取硬盘中的数据，直至读完(每次都读取256个扇区, 直至最后一次读取小于256的"余数"个扇区)
        if((secs_done + 256) <= sec_cnt){
            secs_op = 256;
        } else{
            secs_op = sec_cnt - secs_done;
        }
        // 2. 写入待读入的扇区数和起始扇区号
        select_sector(hd, lba + secs_done, secs_op);

        //3. 将要执行的命令写入到命令寄存器中
        cmd_out(hd->my_channel, CMD_READ_SECTOR); // 马上开始读取数据

        /** 现在硬盘已经开始工作了, 硬盘工作后，当前线程才能阻塞自己，以让出cpu，等待硬盘完成读操作后通过中断处理程序唤醒自己 **/
        sema_down(&hd->my_channel->disk_done);

        // 4. 检测硬盘当前的状态是否可读, 从上一步醒来后开始执行下面的代码
        if(!busy_wait(hd)) {  // 如果失败（说明硬盘没能准备好数据，大概率是坏了）
            char error[64];
            sprintf(error, "%s read sector %d failed!!!\n", hd->name, lba);
            PANIC(error);    // 停止程序运行
        }
        // 5. 把数据从缓冲区读出
        read_from_sector(hd, (void*)((uint32_t)buf + secs_done*512), secs_op);
        secs_done += secs_op;
    }
    lock_release(&hd->my_channel->lock);
}

/* 将buf中sec_cnt扇区数据写入硬盘 */
void ide_write(struct disk* hd, uint32_t lba, void* buf, uint32_t sec_cnt) {
    ASSERT(lba <= max_lba && sec_cnt > 0);
    lock_acquire(&hd->my_channel->lock);

    // 1. 先选择要操作的硬盘
    select_disk(hd);

    uint32_t secs_op;
    uint32_t secs_done = 0;

    while (secs_done < sec_cnt){
        if(secs_done + 256 <= sec_cnt){
            secs_op = 256;
        }else{
            secs_op = sec_cnt - secs_done;
        }
        // 2. 写入待写入的扇区数和起始扇区号
        select_sector(hd, lba + secs_done, secs_op);

        // 3. 将要执行的命令写入到命令寄存器中
        cmd_out(hd->my_channel, CMD_WRITE_SECTOR);    // 准备开始写数据

        // 4. 检测硬盘状态是否可读
        if(!busy_wait(hd)){
            char error[64];
            sprintf(error, "%s write sector %d failed!!!\n", hd->name, lba);
            PANIC(error);
        }
        // 5. 将数据写入到硬盘中
        write2sector(hd, (void*)((uint32_t)buf + secs_done * 512), secs_op);

        /** 此时硬盘才开始工作, 在硬盘响应期间阻塞当前线程 **/
        sema_down(&hd->my_channel->disk_done);

        secs_done += secs_op;
    }
    lock_release(&hd->my_channel->lock);
}

/* 硬盘中断处理程序 */
void intr_hd_handler(uint8_t irq_no) {
    // 中断号必须是这两个中的一个, 分别对应8259A从片的IRQ14、15接口
    ASSERT(irq_no == 0x2e || irq_no == 0x2f);
    uint8_t ch_no = irq_no - 0x2e;    // 得到当前中断是哪个通道发出的, 即结果可用于channels[]索引
    struct ide_channel* channel = &channels[ch_no];
    ASSERT(channel->irq_no == irq_no);
    // 不必担心此次中断是否对应的就是最近一次的expecting_intr, 通道锁保证了这一点(不存在很久之前硬盘发出中断，现在才处理的情况)
    if(channel->expecting_intr){
        channel->expecting_intr = false;
        sema_up(&channel->disk_done);
        // 读取状态寄存器使得硬盘控制器认为此次中断已被处理, 从而硬盘可以产生新的中断
        inb(reg_status(channel));
    }
}

/* 将dst中len个相邻两个字节交换位置后存入buf */
static void swap_pairs_bytes(const char* dst, char* buf, uint32_t len) {
    uint8_t idx;
    for(idx = 0; idx < len; idx += 2) {
        // buf中存储dst中两相邻元素交换位置后的字符串
        buf[idx + 1] = *dst++;
        buf[idx] = *dst++;
    }
    buf[idx] = '\0';
}

/* 获得硬盘参数信息 */
static void identify_disk(struct disk* hd) {
    char id_info[512];
    select_disk(hd);    // 往硬盘所在的ide通道上写入device寄存器状态, 以选择硬盘
    cmd_out(hd->my_channel, CMD_IDENTIFY);  // 向硬盘发送指令后, 硬盘马上开始工作

    /** 硬盘已经开始工作, 阻塞当前线程. 待硬盘处理完成后，通过中断处理程序将自己唤醒 **/
    sema_down(&hd->my_channel->disk_done);

    // 醒来后开始执行下面的代码
    if(!busy_wait(hd)){    // 若失败, 程序悬停！
        char error[64];
        sprintf(error, "%s identify failed!!!\n", hd->name);
        PANIC(error);
    }
    read_from_sector(hd, id_info, 1);
    // 此时id_info数组中已经是硬盘信息了，开始打印
    char buf[64]; // 用于缓存交换相邻字节后的信息
    uint8_t sn_start = 10*2, sn_len = 20, md_start =  27*2, md_len = 40;   // 这些是identify命令返回的信息中的偏移量
    swap_pairs_bytes(&id_info[sn_start], buf, sn_len);
    printk("    disk %s info:\n          SN: %s\n", hd->name, buf);        // 打印硬盘序列号

    memset(buf, 0, sizeof(buf));
    swap_pairs_bytes(&id_info[md_start], buf, md_len);
    printk("      MODULE: %s\n", buf);                                     // 打印硬盘型号

    uint32_t sectors = *(uint32_t*)&id_info[60 * 2];
    printk("      SECTORS: %d\n", sectors);                                // 打印硬盘可供用户使用的扇区数
    printk("      CAPACITY: %dMB\n", sectors * 512 / 1024 / 1024);
}

/* 分区表扫描函数：扫描硬盘hd中"起始地址为ext_lba的一扇区"中的所有分区 */
static void partition_scan(struct disk* hd, uint32_t ext_lba) {
    struct boot_sector* bs = sys_malloc(sizeof(struct boot_sector));    // "动态"申请一扇区大小的内存来存储分区表所在的扇区
    ide_read(hd, ext_lba, bs, 1);    // 将引导扇区的内容读取bs中缓存
    uint8_t part_idx = 0;
    struct partition_table_entry* p = bs->partition_table;    // 令指针p指向分区表数组起始地址

    // 利用指针p遍历分区表4个分区表项
    while (part_idx++ < 4) {
        if(p->fs_type == 0x5){    // 若当前分区表项 指向的是扩展分区, 意味着要递归调用partition_scan
            if(ext_lba_base != 0){ // 此时获取到的分区表项是EBR引导扇区中的
                // 子扩展分区的start_lba是相对于主引导扇区中的总扩展分区地址
                partition_scan(hd, p->start_lba + ext_lba_base);
            } else{    // ext_lba_base为0意味着这是第一次调用partition_scan，即第一次读取MBR所在的扇区
                ext_lba_base = p->start_lba;    // 更新(获得)“总拓展分区”的起始lba地址, 后面的子拓展分区地址都相对于此
                partition_scan(hd, p->start_lba);
            }
        } else if(p->fs_type != 0) {    // 若是其他有效的分区类型(主分区[0x83] 或 逻辑分区[0x66]), 无效的分区比如EBR分区表中的第三第四个表项以及最后一个EBR中的第二，第三，第四个表项
            if(ext_lba == 0) {    // ext_lba若为0, p指向的肯定是MBR中的分区表, 该分区表中只有主分区表项和总拓展分区表项, 而如果是"总拓展分区"表项,会在上面就被处理, 此时必为主分区
                // 将主分区的信息收录到磁盘hd的prim_parts数组中
                hd->prim_parts[p_no].start_lba = ext_lba + p->start_lba;
                hd->prim_parts[p_no].sec_cnt = p->sec_cnt;
                hd->prim_parts[p_no].my_disk = hd;
                list_append(&partition_list, &hd->prim_parts[p_no].part_tag);
                sprintf(hd->prim_parts[p_no].name, "%s%d", hd->name, p_no + 1);
                p_no++;
                ASSERT(p_no < 4); // 0 1 2 3
            } else{    // 此时p指向的必为逻辑分区表项
                hd->logic_parts[l_no].start_lba = ext_lba + p->start_lba;
                hd->logic_parts[l_no].sec_cnt = p->sec_cnt;
                hd->logic_parts[l_no].my_disk = hd;
                list_append(&partition_list, &hd->logic_parts[l_no].part_tag);
                sprintf(hd->logic_parts[l_no].name, "%s%d", hd->name, l_no + 5);	 // 逻辑分区数字是从5开始,主分区是1～4.
                l_no++;
                if (l_no >= 8)    // 只支持8个逻辑分区,避免数组越界
                    return;
            }
        }
        p++;
    }
    sys_free(bs);
}

/* 打印分区信息: 被用在list_traversal中作为回调函数调用, 必须有2个参数 */
static bool partition_info(struct list_elem* pelem, int arg UNUSED) {
    struct partition* part = elem2entry(struct partition, part_tag, pelem);
    printk("   %s start_lba:0x%x, sec_cnt:0x%x\n",part->name, part->start_lba, part->sec_cnt);
	// 在此处return false与函数本身功能无关,只是为了让主调函数list_traversal继续向下遍历元素
    return false;
}


/* 硬盘数据结构初始化 */
void ide_init(){
    printk("ide_init start!\n");
    uint8_t hd_cnt = *((uint8_t*)(0x475));    // 获取硬盘数量, BIOS将硬盘数量写入到了0x475地址中
    ASSERT(hd_cnt > 0);
    list_init(&partition_list);
    channel_cnt = DIV_ROUND_UP(hd_cnt, 2);    // 一个ide通道上有两个硬盘, 根据硬盘数量反推有几个ide通道

    struct ide_channel* channel;
    uint8_t channel_no = 0, dev_no = 0;
    /* 处理每个通道上的硬盘 */
    while (channel_no < channel_cnt){
        channel = &channels[channel_no];              // 取得第channel_no个通道的地址
        sprintf(channel->name, "ide%d", channel_no);

        // (1) 为”当前“ide通道初始化端口基址以及中断号
        switch (channel_no) {
            case 0:
                channel->port_base = 0x1f0;    // ide0通道(即primary通道)的端口基址为0x1f0
                channel->irq_no = 0x20 + 14;   // 从片8259A上倒数第二的中断引脚, 即IRQ14的中断号
                break;
            case 1:
                channel->port_base = 0x170;    // ide1通道
                channel->irq_no = 0x20 + 15;   // 从8259A上的最后一个中断引脚,我们用来响应ide1通道上的硬盘中断
                break;
        }

        channel->expecting_intr = false;     // 还未向硬盘写入指令时, 不期待硬盘发出的中断
        lock_init(&channel->lock);

        // 初始化通道的信号量为0, 目的是向硬盘控制器请求数据后，硬盘驱动sema_down此信号量会阻塞线程
        sema_init(&channel->disk_done, 0);

        register_handler(channel->irq_no, intr_hd_handler); // 为通道注册中断处理函数

      // (2) 分别获取两个硬盘的参数及分区信息
      while (dev_no < 2) {

	    struct disk* hd = &channel->devices[dev_no];
	    hd->my_channel = channel;    // 写入当前通道所属的dev_no个磁盘, 所属哪个通道
	    hd->dev_no = dev_no;         // 写入当前通道所属的dev_no个磁盘, 所属哪个编号
	    sprintf(hd->name, "sd%c", 'a' + channel_no * 2 + dev_no);
	    identify_disk(hd);	 // 获取硬盘参数
	    if (dev_no != 0) {	 // 内核本身的裸硬盘(hd60M.img)不处理
	        partition_scan(hd, 0);  // 扫描该硬盘上的分区
	    }
	    p_no = 0, l_no = 0;    // 遍历下一个盘前, 将p_no和l_no重置为0
	    dev_no++;
        }
        dev_no = 0;			  	   // 将硬盘驱动器号置0,为下一个channel的两个硬盘初始化。
        channel_no++;				   // 下一个channel
    }

   printk("\n   all partition info\n");
   /* 打印所有分区信息 */
   list_traversal(&partition_list, partition_info, (int)NULL);
   printk("ide_init done\n");
}

