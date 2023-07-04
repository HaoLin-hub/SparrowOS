
; ------------------------------  loader and Kernel ----------------------------------
LOADER_BASE_ADDR equ 0x900
LOADER_STACK_TOP equ LOADER_BASE_ADDR
LOADER_START_SECTOR equ 0x2

KERNEL_BIN_BASE_ADDR equ 0x70000
KERNEL_START_SECTOR equ 0x9
KERNEL_ENTRY_POINT equ 0xc0001500

; --------------------------------------  页表配置  ------------------------------------
PAGE_DIR_TABLE_POS equ 0x100000

; ------------------------------- gdt描述符 ----------------------------------------
DESC_G_4K equ 1_00000000000000000000000b   ;GDT高32位的第23位为1，说明段界限的单位以4KB为粒度
DESC_D_32 equ 1_0000000000000000000000b    ;第22位D/B位为1，对代码段来说是D位，在此表示指令中的有效地址和操作数是32位的
DESC_L    equ 0_000000000000000000000b     ;第21位 设置成0表示不设置成64位代码段,保留位暂时忽略~
DESC_AVL  equ 0_00000000000000000000b      ;第20位 是软件可用的 操作系统额外提供的 可不设置

DESC_LIMIT_CODE2 equ 1111_0000000000000000b;第16-19位 段界限的最后四位 平坦模型下需访问整个4G地址空间，故段界限取最大值
DESC_LIMIT_DATA2  equ  DESC_LIMIT_CODE2    ;相同的值  数据段与代码段段界限相同
DESC_LIMIT_VIDEO2 equ 0000_000000000000000b;第16-19位 显存区描述符VIDEO2， 这里的全是0为高位 低位即可表示段基址

DESC_P     equ 1_000000000000000b  ;第15位，P判断段是否存在于内存，针对现在的Cpu来说，段一般都是存在于内存中的(历史遗留)
DESC_DPL_0 equ 00_0000000000000b   ;第13-14位 重量级设置 表示特权级 0-3
DESC_DPL_1 equ 01_0000000000000b  ;0为操作系统 权力最高 3为用户段 用于保护
DESC_DPL_2 equ 10_0000000000000b
DESC_DPL_3 equ 11_0000000000000b

DESC_S_sys        equ  0_000000000000b           ;第12位为0 则表示系统段
DESC_S_CODE       equ  1_000000000000b           ;第12位为1 则表示非系统段，即普通的内存段，这里用于表示数据段
DESC_S_DATA       equ  DESC_S_CODE               ;第12位为1 则表示非系统段，即普通的内存段,这里用于表示代码段

DESC_TYPE_CODE    equ  1000_00000000b            ;第9-11位表示该段状态XCRA=1000表示只执行代码段，参考P153 表4-10
DESC_TYPE_DATA    equ  0010_00000000b            ;第9-11位表示该段状态XEWA=0010表示可读写数据段


;对代码段描述符高位4字节进行初始化 (0x00共8位 <<24 共32位初始化0)，0x00是段基址的第16~23位，但注意显存段基址的16~23位是0x0B
DESC_CODE_HIGH4   equ  (0x00<<24) + DESC_G_4K + DESC_D_32 + \
DESC_L + DESC_AVL + DESC_LIMIT_CODE2 + \
DESC_P + DESC_DPL_0 + DESC_S_CODE + DESC_TYPE_CODE + 0X00

;对数据段描述符高位4字节进行初始化
DESC_DATA_HIGH4   equ  (0x00<<24) + DESC_G_4K + DESC_D_32 + \
DESC_L + DESC_AVL + DESC_LIMIT_DATA2 + \
DESC_P + DESC_DPL_0 + DESC_S_DATA + DESC_TYPE_DATA + 0X00

;显存段描述符高位4字节初始化
DESC_VIDEO_HIGH4   equ (0x00<<24) + DESC_G_4K + DESC_D_32 + \
DESC_L + DESC_AVL + DESC_LIMIT_VIDEO2 + \
DESC_P + DESC_DPL_0 + DESC_S_DATA + DESC_TYPE_DATA + 0X0b

; --------------------------  选择子属性 -------------------------------------------------
RPL0    equ 00b
RPL1    equ 01b
RPL2    equ 10b
RPL3    equ 11b
TI_GDT  equ 000b
TI_LDT  equ 100b

;----------------   页表相关属性    --------------
PG_P  equ   1b
PG_RW_R	 equ  00b 
PG_RW_W	 equ  10b 
PG_US_S	 equ  000b 
PG_US_U	 equ  100b

;------------------------ program type 定义 -----------------------------
PT_NULL equ 0