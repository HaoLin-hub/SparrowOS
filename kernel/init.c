#include "init.h"
#include "print.h"
#include "interrupt.h"
#include "../device/timer.h"
#include "memory.h"
#include "thread.h"
#include "console.h"
#include "keyboard.h"
#include "tss.h"
#include "syscall-init.h"
#include "ide.h"
#include "fs.h"

/*负责初始化所有模块 */
void init_all() {
   put_str("init_all\n");
   idt_init();   //初始化中断
   mem_init();	  // 初始化内存管理系统
   thread_init(); // 初始化线程环境
   timer_init();  // 初始化PIT(放在thread_init后是因为只有先初始化了主线程，才有"当前线程"给时钟中断处理函数处理)
   console_init(); // 控制台初始化最好放在开中断之前
   keyboard_init();  // 键盘初始化
   tss_init();       // tss初始化
   syscall_init();   // 初始化系统调用
   intr_enable();    // 后面的ide_init需要打开中断
   ide_init();	     // 初始化硬盘
   filesys_init();   // 初始化文件系统
}
