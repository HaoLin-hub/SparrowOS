#include "shell.h"
#include "stdint.h"
#include "file.h"
#include "syscall.h"
#include "stdio.h"
#include "global.h"
#include "assert.h"
#include "string.h"
#include "buildin_cmd.h"
#include "pipe.h"

#define cmd_len 128        // 最大支持键入128个字符的命令输入
#define MAX_ARG_NR 16      // 在一行命令中, 最多支持15个参数

/* 数组cmd_line存储输入的命令 */
static char cmd_line[cmd_len] = {0};
/* 数组cwd_cache用来存储当前的目录名, 主要是用在命令提示符中, 它由以后实现的cd命令来维护 */
char cwd_cache[MAX_PATH_LEN] = {0};

char final_path[MAX_PATH_LEN];
char* argv[MAX_ARG_NR];

/* 输出命令提示符, 也就是咱们登录shell后, 命令行中显示的主机名等 */
void print_prompt(void) {
    printf("[rabbit@localhost %s]$ ", cwd_cache);
}

/* 从键盘缓冲区中最多读入count个字节到buf */
static void readline(char* buf, int32_t count) {
    assert(buf != NULL && count > 0);
    char* pos = buf;    // 通过将pos指向buf, 通过循环调用"read系统调用"读入1个字符到pos中, 最终buf充满count个字符
    while (read(stdin_no, pos, 1) != -1 && (pos - buf) < count) {
        switch (*pos) {
            // 找到回车符or换行符后可认为命令已键入完毕, 直接返回
            case '\n':
            case '\r':
                *pos = 0;    // 添加cmd_line的终止字符0（ascii 为0, 表示'\0'结束符）
                putchar('\n');
                return;

            // 处理退格键
            case '\b':
                if(cmd_line[0] != '\b') {  // 阻止删除非本次输入的信息（否则将删掉命令提示符和之前的输出）
                    --pos;
                    putchar('\b');
                }
                break;

            // ctrl + l 清屏
            case 'l' - 'a':
                *pos = 0;        // 先将当前的字符'l'-'a'置0(相当于截断buf中的命令行字符串)
                clear();         // 再将屏幕清空
                print_prompt();  // 打印命令提示符
                printf("%s", buf);     // 将原来保存在buf中的命令行字符串再次打印到屏幕
                break;

            // ctrl + u 清空命令行输入
            case 'u' - 'a':
                while (buf != pos) {
                    putchar('\b');
                    *pos = 0;
                    pos--;
                }
                break;
                
            // 非控制键则输出字符
            default:
                putchar(*pos);
                pos++;
        }
    }
    // 程序要是运行都这, 说明命令键入的字符已超过最大值
    printf("readline: can`t find enter_key in the cmd_line, max num of char is 128\n");
}

/* 解析输入的命令：这里的实现只是以参数token为分隔符，分割命令行中输入的字符串中的单词, 将解析出的单词保存在argv中, 成功返回单词个数, 失败返回-1 */
static int32_t cmd_parse(char* cmd_str, char** argv, char token) {
    assert(cmd_str != NULL);
    // 清空argv数组
    int32_t arg_idx = 0;
    while(arg_idx < MAX_ARG_NR) {
        argv[arg_idx] = NULL;
        arg_idx++;
    }
    // 令指针next指向cmd_str, next用于处理命令行字符串中的每一个字符
    char* next = cmd_str;
    int32_t argc = 0;
    while (*next) {    // 遍历命令行的每一个字符
        while (*next == token) {  // 用于跨过cmd_str中的空格
            next++;
        }
        if(*next == 0) { // 如果当前已遍历到命令行字符串的末尾, 直接退出循环
            break;
        }
        argv[argc] = next;  // 每找出一个字符串就将其在cmd_str中的起始next存储到argv数组

        // 循环处理命令行中的每个命令字以及参数
        while (*next && *next != token){  // 直到next指向分隔符
            next++;
        }
        // 如果此时命令行字符串未结束, 直接将token字符替换为0,做为一个单词的结束,并将字符指针next指向下一个字符
        if (*next != 0){
            *next = 0;
            next++;
        }
        /* 避免argv数组访问越界,参数过多则返回0 */
        if (argc > MAX_ARG_NR) {
            return -1;
        }
        argc++;
    }
    return argc;
}

/* 判断命令行键入的命令并执行之 */
static void cmd_execute(uint32_t argc, char** argv){
    // argv[0]被认为是命令
    if (!strcmp("ls", argv[0])) {
        buildin_ls(argc, argv);
    } else if (!strcmp("cd", argv[0])) {
        if (buildin_cd(argc, argv) != NULL) {
            memset(cwd_cache, 0, MAX_PATH_LEN);
            strcpy(cwd_cache, final_path);
        }
    } else if (!strcmp("pwd", argv[0])) {
        buildin_pwd(argc, argv);
    } else if (!strcmp("ps", argv[0])) {
        buildin_ps(argc, argv);
    } else if (!strcmp("clear", argv[0])) {
        buildin_clear(argc, argv);
    } else if (!strcmp("mkdir", argv[0])){
        buildin_mkdir(argc, argv);
    } else if (!strcmp("rmdir", argv[0])){
        buildin_rmdir(argc, argv);
    } else if (!strcmp("rm", argv[0])) {
        buildin_rm(argc, argv);
    } else if (!strcmp("help", argv[0])) {
        buildin_help(argc, argv);
    } else {    // 如果是外部命令, 则需要从磁盘上加载
        int32_t pid = fork();
        if(pid) {    // 父进程
            int32_t status;
            int32_t child_pid = wait(&status);  // 此时若子进程没有exit, my_shell将会被阻塞, 不再响应键入的命令
            if (child_pid == -1) {
                panic("my_shell: no child\n");
            }
            printf("child_pid %d, it's status: %d\n", child_pid, status);
        } else {    // 子进程
            // 获取可执行文件argv[0], 将其转化为绝对路径格式
            make_clear_abs_path(argv[0], final_path);
            argv[0] = final_path;
            // 判断文件是否存在
            struct stat file_stat;
            memset(&file_stat, 0, sizeof(struct stat));
            if(stat(argv[0], &file_stat) == -1) {
                printf("my_shell: cannot access %s: No such file or directory\n", argv[0]);
                exit(-1);
            } else {
                execv(argv[0], argv);
            }
        }
    }
}

int32_t argc = -1;
/* 简单的shell */
void my_shell(void) {
    cwd_cache[0] = '/';    // 先将当前工作目录缓存cwd_cache置为根目录'/'
    cwd_cache[1] = 0;
    while (1){
        print_prompt();
        memset(final_path, 0, MAX_PATH_LEN);
        memset(cmd_line, 0, cmd_len);
        readline(cmd_line, cmd_len);
        if(cmd_line[0] == 0) {    // 若只键入了一个回车, 当做无事发生, 新的一行打印命令提示符，继续等待用户输入
            continue;
        }

        // 针对管道的处理
        char* pipe_symbol = strchr(cmd_line, '|');    // 寻找管道字符“|”, 如果找到则将其字符地址赋给pipe_symbol
        if(pipe_symbol) {    // 为支持多重管道操作, cmd1的标准输出和cmdn的标准输入需要单独处理
            // step1: 生成管道
            int32_t fd[2] = {-1};  // fd[0]用于输入, fd[1]用于输出
            pipe(fd);
            fd_redirect(1, fd[1]); // 至此程序的输出都写到管道中了(除最后一个命令外)

            // step2: 解析第1个命令并执行
            char* each_cmd = cmd_line;
            pipe_symbol = strchr(each_cmd, '|');
            *pipe_symbol = 0;    // 分割出第一个命令
            argc = -1;
            argc = cmd_parse(each_cmd, argv, ' ');
            cmd_execute(argc, argv);

            each_cmd = pipe_symbol + 1;    // 跨过'|' 处理下一个命令
            fd_redirect(0, fd[0]);         // 从第二个命令始, 标准输入都被重定向到fd[0], 使之指向内核环形缓冲区

            // step3: 循环处理中间的命令, 此时它们的标准输入和输出均已指向管道
            while ((pipe_symbol = strchr(each_cmd, '|')) != NULL) {
                *pipe_symbol = 0;
                argc = -1;
                argc = cmd_parse(each_cmd, argv, ' ');
                cmd_execute(argc, argv);
                each_cmd = pipe_symbol + 1;
            }

            // step4: 此时到达最后一个命令, 将程序的标准输出恢复为屏幕
            fd_redirect(1, 1);

            // step5: 解析并执行最后一个命令, 并将程序的标准输入恢复为键盘
            argc = -1;
            argc = cmd_parse(each_cmd, argv, ' ');
            cmd_execute(argc, argv);
            fd_redirect(0, 0);

            // step6: 关闭管道
            close(fd[0]);
            close(fd[1]);
        } else {
            argc = -1;
            argc = cmd_parse(cmd_line, argv, ' ');
            if(argc == -1){
                printf("number of arguments exceed %d\n", MAX_ARG_NR);
                continue;
            }
            cmd_execute(argc, argv);
        }
    }
    panic("my_shell: should not be here");
}