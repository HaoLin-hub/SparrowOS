#include "string.h"
//#include "global.h"
#include "assert.h"

/* 将dst_起始的size个字节置为value */
void memset(void* dst_, uint8_t value, uint32_t size){
    assert(dst_ != NULL);
    // 将dst_转为8位整型, 代表按字节取址
    uint8_t* dst = (uint8_t*)dst_;
    while (size-- > 0){
        *dst = value;
        dst++;
    }
}

/* 将src_起始的size个字节复制到dst_ */
void memcpy(void* dst_, const void* src_, uint32_t size){
    assert(dst_ != NULL && src_ != NULL);
    uint8_t* dst = dst_;
    const uint8_t* src = src_;
    while (size--){
        *dst = *src;
        dst++;
        src++;
    }
}

/* 连续比较以地址a_ 和地址 b_ 开头的size个字节，若相等则返回0， 若a_大于b_, 返回+1，否则返回-1 */
int memcmp(const void* a_, const void* b_, uint32_t size){
    const char* a = a_;
    const char* b = b_;
    assert(a != NULL || b != NULL);
    while (size-- > 0){
        if(*a != *b){
            return *a > *b ? 1 : -1;
        }
        a++;
        b++;
    }
    return 0;
}

/* 将字符串从src_ 复制到 dst_ */
char* strcpy(char* dst_, const char* src_){
    assert(dst_ != NULL && src_ != NULL);
    char* r = dst_;    // 用来返回目的字符串起始地址
    while ((*dst_++ = *src_++));    // 开始从src_复制直到*src_为NULL
    return r;
}

/* 返回字符串长度 */
uint32_t strlen(const char* str){
    assert(str != NULL);
    const char* p = str;
    // 直到字符串末尾'/0'
    while (*p++);

    return (p - str - 1);    // 记得减1, ‘/0’不计入字符串长度
}

/* 比较两个字符串, 若a_中的字符大于b_中的字符返回1, 相等返回0, 小于返回-1 */
int8_t strcmp(const char* a, const char* b){
    assert(a != NULL && b != NULL);
    // 字符串结束表示为'/0'对应ASCII为0
    while (*a != 0 && *a == *b){
        a++;
        b++;
    }
    return *a < *b ? -1 : (*a > *b);
}

/* 从左到右查找字符串str中首次出现字符ch的地址 */
char* strchr(const char* str, const uint8_t ch){
    assert(str != NULL);
    while (*str != 0){
        if(*str == ch){
            return (char*)str;    // 需要强制转化成与返回值类型一样，否则编译器会报const属性丢失
        }
        str++;
    }
    return NULL;
}

/* 从右到左查找字符串str中首次出现字符ch的地址,注意！是地址,不是下标 */
char* strrchr(const char* str,const uint8_t ch)
{
    assert(str != NULL);
    const char* last_char = NULL;
    while(*str != 0)
    {
        if(ch == *str)	last_char = str;
        str++;
    }
    return (char*)last_char;
}

/* 将字符串src_拼接到dst_后,将回拼接的串地址 */
char* strcat(char* dst_, const char* src_) {
   assert(dst_ != NULL && src_ != NULL);
   char* str = dst_;
   while (*str++);
   --str;      // 别看错了，--str是独立的一句，并不是while的循环体
   while((*str++ = *src_++));	 // 当*str被赋值为0时,此时表达式不成立,正好添加了字符串结尾的0.
   return dst_;
}

/* 在字符串str中查找指定字符ch出现的次数 */
uint32_t strchrs(const char* str, uint8_t ch) {
   assert(str != NULL);
   uint32_t ch_cnt = 0;
   const char* p = str;
   while(*p != 0) {
      if (*p == ch) {
	 ch_cnt++;
      }
      p++;
   }
   return ch_cnt;
}