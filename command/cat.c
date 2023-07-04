#include "syscall.h"
#include "stdio.h"
#include "string.h"
int main(int argc, char** argv) {
   if (argc > 2) {
      printf("cat: argument error\n");
      exit(-2);
   }

   // 如果cat程序没有参数, 则直接调用read系统调用从键盘获取数据
   if (argc == 1) {
      char buf[512] = {0};
      read(0, buf, 512);
      printf("%s",buf);
      exit(0);
   }

    char abs_path[512] = {0};  // 存储参数的绝对路径

   int buf_size = 1024;        // 申请1024字节的内存用作缓冲区
   void* buf = malloc(buf_size);
   if (buf == NULL) { 
      printf("cat: malloc memory failed\n");
      return -1;
   }
   // 处理参数文件的路径, 将其转换为绝对路径后存入abs_path数组
   if (argv[1][0] != '/') {
      getcwd(abs_path, 512);
      strcat(abs_path, "/");
      strcat(abs_path, argv[1]);
   } else {
      strcpy(abs_path, argv[1]);
   }
   // 以“只读”方式打开参数文件
   int fd = open(abs_path, O_RDONLY);
   if (fd == -1) { 
      printf("cat: open: open %s failed\n", argv[1]);
      return -1;
   }
   // 读取参数文件, 直到read返回-1, 也就是一直读到文件末尾
   int read_bytes= 0;
   while (1) {
      read_bytes = read(fd, buf, buf_size);
      if (read_bytes == -1) {
         break;
      }
      write(1, buf, read_bytes);
   }
   free(buf);
   close(fd);
   return 66;
}
