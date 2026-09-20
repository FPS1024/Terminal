/* 平台相关：越狱路径探测、shell 选择、环境变量组装 */
#ifndef PLATFORM_H
#define PLATFORM_H
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 无根越狱返回 "/var/jb"，传统越狱返回 "" */
const char *jb_root(void);
/* 拼接越狱前缀路径，写进 out */
void jb_path(char *out, size_t n, const char *sub);
const char *default_home(void);
/* 找一个可用的 shell，返回静态字符串 */
const char *default_shell(void);
/* 组装 envp（以 NULL 结尾的字符串数组，调用方用 free_env 释放） */
char **build_env(const char *shell, const char *term, const char *lang, int cols, int rows);
void free_env(char **env);
/* 挑一个本机可用的 UTF-8 locale（libc 实际可用性判断，不是看目录） */
const char *pick_lang(void);
/* 该名字是否六个分类都有数据（能不能安全写进 LC_ALL） */
int lang_is_full(const char *name);
const char *build_path(void);
#ifdef __cplusplus
}
#endif
#endif
