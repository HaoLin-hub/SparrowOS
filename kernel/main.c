#include "print.h"
#include "init.h"
#include "thread.h"
#include "interrupt.h"
#include "console.h"
#include "process.h"
#include "syscall-init.h"
#include "syscall.h"
#include "stdio.h"
#include "memory.h"
#include "dir.h"
#include "fs.h"
#include "assert.h"
#include "shell.h"
#include "ide.h"
#include "stdio-kernel.h"

void init(void);

int main(void) {
   put_str("I am kernel\n");
   init_all();
   /***********    写入应用程序到磁盘       ***********/
   uint32_t file_size = 4777;
   uint32_t sec_cnt = DIV_ROUND_UP(file_size, 512);
   // 指定要操作的设备是sda, 即第0个ide通道上的第0块硬盘
   struct disk* sda = &channels[0].devices[0];
   void* prog_buf = sys_malloc(file_size);
   // 将程序从硬盘中读入到prog_buf中
   ide_read(sda, 300, prog_buf, sec_cnt);

   // 在根目录下创建文件prog_no_arg, 并将prog_buf中的数据写入prog_no_arg, 形成程序文件
   int32_t fd = sys_open("/prog_no_arg", O_CREAT | O_RDWR);
   if(fd != -1) {
       if(sys_write(fd, prog_buf, file_size) == -1) {
           printk("file write error!\n");
           while (1);
       }
   }
   /*********************************************/

   cls_screen();
   console_put_str("[linhao@localhost /]$ ");
   thread_exit(running_thread(), true);
   return 0;
}

/* init进程 */
void init(void) {
   uint32_t ret_pid = fork();
   if(ret_pid) {  // 父进程
      int status;
      int child_pid;
      // init在此处不停地回收僵尸进程
       while (1) {
           child_pid = wait(&status);
           printf("I am init, My pid is 1, I recieve a child, It's pid is %d, status is %d\n", child_pid, status);
       }
   } else {	  // 子进程
      my_shell();
   }
   panic("init: should not be here");
}
