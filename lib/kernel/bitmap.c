#include "bitmap.h"
#include "stdint.h"
#include "string.h"
#include "print.h"
#include "interrupt.h"
#include "debug.h"

/* 将位图bitmap初始化 */
void bitmap_init(struct bitmap* btmp){
    // 根据位图的字节大小btmp_bytes_len将位图的每一个字节用0填充
    memset(btmp->bits, 0, btmp->btmp_bytes_len);
}

/* 判断bit_idx位是否为1, 若为1则返回true, 否则返回false */
bool bitmap_scan_test(struct bitmap* btmp, uint32_t bit_idx){
    uint32_t byte_idx = bit_idx / 8;    // 向下取整索引数组下标
    uint32_t bit_odd = bit_idx % 8;     // 取余用于索引数组内的位
    return (btmp->bits[byte_idx]) & (BITMAP_MASK << bit_odd);
}

/* 在位图中申请连续cnt个位, 成功, 则返回其起始下标, 失败返回-1 */
int bitmap_scan(struct bitmap* btmp, uint32_t cnt){
    uint32_t idx_byte = 0;    // 记录空闲位所在的字节
    /* 先才用暴力法：逐字节比较 */
    while ((0xff == btmp->bits[idx_byte]) && (idx_byte < btmp->btmp_bytes_len)){
        // 0xff表示该字节内的位对应的资源都已分配完毕, 已无空闲位, 向下一字节继续寻找
        idx_byte++;
    }
    ASSERT(idx_byte < btmp->btmp_bytes_len);
    if(idx_byte == btmp->btmp_bytes_len){    // 若内存池中已找不到可用空间
        return -1;
    }

    // 若在位图数组范围内的某字节内找到了空闲位, 便在该字节内逐位比对, 返回空闲位的索引
    int idx_bit = 0;
    while (btmp->bits[idx_byte] & (uint8_t)(BITMAP_MASK << idx_bit)){    // 一旦在bits[idx_byte]中遇到0位, 则退出循环
        idx_bit++;
    }

    int bit_idx_start = idx_byte * 8 + idx_bit;    // 空闲位在位图内的下标
    if(cnt == 1){
        return bit_idx_start;
    }
    uint32_t bit_left = (btmp->btmp_bytes_len * 8 - bit_idx_start);    // 从bit_idx_start开始直到位图末位还有多少个位可供判断
    uint32_t next_bit = bit_idx_start + 1;
    uint32_t count = 1;    // 记录已找到的空闲位的个数

    bit_idx_start = -1;    // 要返回的起始位图索引, 先假设找不到cnt连续空闲位
    // 避免访问位图外的内存
    while (bit_left-- > 0){
        if(!(bitmap_scan_test(btmp, next_bit))){    // 若next_bit为0
            count++;
        }else{
            // 重置count为0
            count = 0;
        }
        if(count == cnt){    // 若找到连续的cnt个空位
            bit_idx_start = next_bit - cnt + 1;
            break;
        }
        next_bit++;
    }

    return bit_idx_start;
}

/* 将位图btmp的bit_idx位设置为value */
void bitmap_set(struct bitmap* btmp, uint32_t bit_idx, int8_t value){
    ASSERT((value == 0) || (value == 1));
    uint32_t byte_idx = bit_idx / 8;
    uint32_t bit_odd = bit_idx % 8;

    if(value == 1){
        btmp->bits[byte_idx] |= (BITMAP_MASK << bit_odd);
    }else{
        btmp->bits[byte_idx] &= ~(BITMAP_MASK << bit_odd);
    }
}