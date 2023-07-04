#include "pipe.h"
#include "memory.h"
#include "fs.h"
#include "file.h"
#include "ioqueue.h"
#include "thread.h"

/* 判断文件描述符local_fd是否为管道 */
bool is_pipe(uint32_t local_fd) {
    uint32_t global_fd = fd_local2global(local_fd);
    return file_table[global_fd].fd_flag == PIPE_FLAG;
}

/* 创建管道, 成功返回0, 失败返回-1 */
int32_t sys_pipe(int32_t pipefd[2]) {
    int32_t global_fd = get_free_slot_in_global();
    // 申请一页内核内存作为环形缓冲区, 并进行初始化
    file_table[global_fd].fd_inode = get_kernel_pages(1);
    if(file_table[global_fd].fd_inode == NULL){
        return -1;
    }

    ioqueue_init((struct ioqueue*)file_table[global_fd].fd_inode);

    // 将fd_flag复用为管道标志
    file_table[global_fd].fd_flag = PIPE_FLAG;
    // 将fd_pos复用为管道打开数
    file_table[global_fd].fd_pos = 2;
    pipefd[0] = pcb_fd_install(global_fd);
    pipefd[1] = pcb_fd_install(global_fd);

    return 0;
}

/* 从管道中读数据 */
uint32_t pipe_read(int32_t fd, void* buf, uint32_t count) {
    char* buffer = buf;
    uint32_t bytes_read = 0;
    uint32_t global_fd = fd_local2global(fd);

    // 获取管道的环形缓冲区
    struct ioqueue* ioq = (struct ioqueue*)file_table[global_fd].fd_inode;

    // 选择本次从ioq中读取count和ioq_len中较小的数据读取量, 避免阻塞
    uint32_t ioq_len = ioq_length(ioq);
    uint32_t size = ioq_len > count?count : ioq_len;
    while (bytes_read < size) {
        *buffer = ioq_getchar(ioq);
        bytes_read++;
        buffer++;
    }
    return bytes_read;
}

/* 从管道中写数据 */
uint32_t pipe_write(int32_t fd, const void* buf, uint32_t count) {
    char* buffer = buf;
    uint32_t bytes_write = 0;
    uint32_t global_fd = fd_local2global(fd);

    // 获取管道的环形缓冲区
    struct ioqueue* ioq = (struct ioqueue*)file_table[global_fd].fd_inode;

    // 选择本次往ioq中写入count和ioq_len中较小的数据写入量, 避免阻塞
    uint32_t ioq_left = bufsize - ioq_length(ioq);
    uint32_t size = ioq_left > count?count : ioq_left;

    while (bytes_write < size){
        ioq_putchar(ioq, *buffer);
        buffer++;
        bytes_write++;
    }
    return bytes_write;
}

/* 将文件描述符old_local_fd重定向为new_local_fd */
void sys_fd_redirect(uint32_t old_local_fd, uint32_t new_local_fd){
    struct task_struct* cur = running_thread();
    // 特殊情况：要恢复标准输入or输出, 直接覆盖即可(因为fd_table[0~2] = 0~2)
    if(new_local_fd < 3) {
        cur->fd_table[old_local_fd] = new_local_fd;
    } else {
        uint32_t new_global_fd = cur->fd_table[new_local_fd];
        cur->fd_table[old_local_fd] = new_global_fd;
    }
}