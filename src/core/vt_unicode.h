/*
 * vt_unicode.h - UTF-8 helpers and East-Asian aware cell widths.
 * 终端里所有"一个字符占几列"的判断都走这里，中文能不能对齐全看它。
 */
#ifndef VT_UNICODE_H
#define VT_UNICODE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define VT_REPLACEMENT_CHAR 0xFFFDu
/* 返回消耗的字节数(>=1)。非法序列返回 1 且 *cp = U+FFFD。 */
int vt_u8_decode(const uint8_t *s, size_t n, uint32_t *cp);
/* 返回写入 out 的字节数(1..4)。 */
int vt_u8_encode(uint32_t cp, uint8_t out[4]);
/* 该码点编码成 UTF-8 需要几个字节。 */
int vt_u8_len(uint32_t cp);
/*
 * 一个码点占用的终端列数：
 *   0 = 组合字符 / 零宽 / ZWJ / 变体选择符
 *   1 = 半角
 *   2 = 全角、汉字、假名、韩文、大部分 emoji
 * 控制字符返回 -1，调用方自己决定怎么处理。
 */
int vt_cell_width(uint32_t cp);
#ifdef __cplusplus
}
#endif
#endif /* VT_UNICODE_H */
