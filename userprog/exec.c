#include "exec.h"
#include "thread.h"
#include "stdio-kernel.h"
#include "fs.h"
#include "string.h"
#include "global.h"
#include "memory.h"

extern void intr_exit(void);
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* 32位elf头 */
struct Elf32_Ehdr {
    unsigned char e_ident[16];    // 用来表示elf字符等信息, 开头4字节的内容固定不变, 是elf文件的魔数
    Elf32_Half e_type;            // 用来指定elf目标文件的类型
    Elf32_Half e_machine;         // 用来描述elf目标文件的体系结构类型, 即描述该文件要在哪种硬件(机器)平台上运行
    Elf32_Word e_version;         // 表示版本信息
    Elf32_Addr e_entry;           // 用来指明操作系统运行该程序时, 将控制权转交到的“虚拟地址”
    Elf32_Off  e_phoff;           // 指明程序头表(program header table)在文件内的偏移量
    Elf32_Off  e_shoff;           // 指明节头表(section header table)在文件内的偏移量
    Elf32_Word e_flags;           // 指明与处理器相关的标志(本项目未用到)
    Elf32_Half e_ehsize;          // 指明elf header的字节大小
    Elf32_Half e_phentsize;       // 指明程序头表(program header table)中每个entry的字节大小, 即每个用来描述段信息的数据结构的字节大小
    Elf32_Half e_phnum;           // 用来指明程序头表中的条目的数量, 即段的个数
    Elf32_Half e_shentsize;       // 指明节头表(section header table)中每个entry的字节大小, 即每个用来描述节信息的数据结构的字节大小
    Elf32_Half e_shnum;           // 用来指明节头表中条目的数量, 即节的个数
    Elf32_Half e_shstrndx;        // 用来指明string name table 在节头表中的索引index
};

/* 程序头表Program header中的"条目"的数据结构, 就是段描述头 */
struct Elf32_Phdr {
    Elf32_Word p_type;		 // 指明程序中该段的类型
    Elf32_Off  p_offset;     // 指明本段在“文件”内的起始偏移字节
    Elf32_Addr p_vaddr;      // 指明本段在“内存”中的起始虚拟地址
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;     // 指明本段在elf文件中的大小
    Elf32_Word p_memsz;      // 指明本段在“内存”中的大小
    Elf32_Word p_flags;      // 指明本段相关的标志(PF_X: 1 本段具有可执行权限, PF_W: 2 本段具有可写权限, PF_R: 4 本段具有可读权限)
    Elf32_Word p_align;      // 指明本段在文件和内存中的对齐方式, 0/1 表示不对齐, 否则表示以2的幂次数对齐
};

/* 段类型 */
enum segment_type {
    PT_NULL,            // 忽略
    PT_LOAD,            // 可加载程序段
    PT_DYNAMIC,         // 动态加载信息
    PT_INTERP,          // 动态加载器名称
    PT_NOTE,            // 一些辅助信息
    PT_SHLIB,           // 保留
    PT_PHDR             // 程序头表
};

/* 将文件描述符fd指向的文件中, 偏移量为offset, 大小为filesz的段加载到虚拟地址为vaddr的内存, 成功返回1, 失败返回0 */
static bool segment_load(int32_t fd, uint32_t offset, uint32_t filesz, uint32_t vaddr) {
    uint32_t vaddr_first_page = vaddr & 0xfffff000;                  // vaddr地址所在的页框的起始地址
    uint32_t size_in_first_page = PG_SIZE - (vaddr & 0x00000fff);    // 加载到内存后, 文件在第一个页框中占用的字节大小
    // 若一个页框容不下该段
    uint32_t occupy_pages = 0;
    if (filesz > size_in_first_page) {
        uint32_t left_size = filesz - size_in_first_page;
        occupy_pages = DIV_ROUND_UP(left_size, PG_SIZE) + 1;    // +1 指加上占用的第一个页框，最后得到该段总共占用的页框个数
    } else {
        occupy_pages = 1;
    }
    // 为进程分配内存
    uint32_t page_idx = 0;
    uint32_t vaddr_page = vaddr_first_page;
    while (page_idx < occupy_pages) {
        // 试图申请对应的虚拟页框
        uint32_t* pde = pde_ptr(vaddr_page);
        uint32_t* pte = pte_ptr(vaddr_page);
        // 如果pde 或 pte不存在就从虚拟内存池中分配内存(pde的判断要在pte之前, 0x00000001指的是 存在位P)
        if(!(*pde & 0x00000001) || !(*pte & 0x00000001)) {
            if(get_a_page(PF_USER, vaddr_page) == NULL) {
                return false;
            }
        }
        // 如果原进程的页表已经分配了相应的页框(可利用对应的物理页,直接覆盖进程体), 就不申请新的虚拟内存了, 继续“试图”分配下一个虚拟页框
        vaddr_page += PG_SIZE;
        page_idx++;
    }
    // 至此, 确保段所需要的内存都已得到分配, 从文件系统上加载用户进程到内存之前, 先通过lseek函数将文件指针定位到段在文件中的偏移地址
    sys_lseek(fd, offset, SEEK_SET);
    // 将该段读入到虚拟地址vaddr处， 自此一个段就被加载到内存中了
    sys_read(fd, (void*)vaddr, filesz);

    return true;
}

/* 从文件系统上加载pathname指向的用户程序, 成功则返回程序的起始地址, 否则返回-1 */
static int32_t load(const char* pathname) {
    int32_t ret = -1;    // 默认返回值
    struct Elf32_Ehdr elf_header;
    struct Elf32_Phdr prog_header;
    memset(&elf_header, 0, sizeof(struct Elf32_Ehdr));

    int32_t fd = sys_open(pathname, O_RDONLY);
    if (fd == -1) {
        printk("the file is not exist\n");
        return -1;
    }
    // 从fd指向的文件开头中读取elf header存入当前数据结构elf_header中, 并比较读取的内容大小是否与结构体大小相同
    if(sys_read(fd, &elf_header, sizeof(struct Elf32_Ehdr)) != sizeof(struct Elf32_Ehdr)) {
        ret = -1;
        goto done;
    }
    // 校验elf格式
    if(memcmp(elf_header.e_ident, "\177ELF\1\1\1", 7) || elf_header.e_type != 2 || elf_header.e_machine != 3 || elf_header.e_version != 1 || elf_header.e_phnum > 1024 || elf_header.e_phentsize != sizeof(struct Elf32_Phdr)) {
        ret = -1;
        goto done;
    }
    // 程序头表(program header table)在文件中的起始偏移地址
    Elf32_Off prog_header_offset = elf_header.e_phoff;
    Elf32_Half prog_header_size = elf_header.e_phentsize;

    // 遍历所有程序头
    uint32_t prog_idx = 0;
    while (prog_idx < elf_header.e_phnum) {
        memset(&prog_header, 0, prog_header_size);
        // 将文件的指针定位到程序头
        sys_lseek(fd, prog_header_offset, SEEK_SET);

        // 只获取程序头, 从文件中读取到结构体变量中
        if (sys_read(fd, &prog_header, prog_header_size) != prog_header_size) {
            ret = -1;
            goto done;
        }
        // 如果是可加载段， 就调用segment_load加载到内存
        if (prog_header.p_type == PT_LOAD) {
            if(!segment_load(fd, prog_header.p_offset, prog_header.p_filesz, prog_header.p_vaddr)) {
                ret = -1;
                goto done;
            }
        }
        // 更新下一个程序头的文件偏移量
        prog_header_offset += elf_header.e_phentsize;
        prog_idx++;
    }
    ret = elf_header.e_entry;

    done:
    sys_close(fd);
    return ret;
}

/* 用path指向的程序替换当前进程, argv[]是传给可执行文件的参数, 失败返回-1, 成功则没有机会返回 */
int32_t sys_execv(const char* path, const char* argv[]) {
    // 统计出参数个数, 存放到遍历argc中
    uint32_t argc = 0;
    while (argv[argc]) {
        argc++;
    }
    // 获取加载程序到内存中的起始虚拟地址
    int32_t entry_point = load(path);
    if(entry_point == -1) {
        return -1;    // 加载失败返回-1
    }
    // 修改进程名
    struct task_struct* cur = running_thread();
    memcpy(cur->name, path, TASK_NAME_LEN);
    cur->name[TASK_NAME_LEN-1] = 0;
    // 将内核栈(里的中断栈)中的内容替换为新进程的参数, 并准备从intr_exit返回从而运行新进程
    struct intr_stack* intr_0_stack = (struct intr_stack*)((uint32_t)cur + PG_SIZE - sizeof(struct intr_stack));
    intr_0_stack->ebx = (int32_t)argv;
    intr_0_stack->ecx = argc;

    intr_0_stack->eip = (void*)entry_point;
    intr_0_stack->esp = (void*)0xc0000000;

    // 将新进程的内核栈地址赋给esp, exec不同于fork, 为使得新进程更快被执行, 直接立即从中断返回
    asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g"(intr_0_stack) : "memory");
    return 0;
}