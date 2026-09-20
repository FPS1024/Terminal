#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>
#include <locale.h>
#include <xlocale.h>
#include <wchar.h>

/* 由 Makefile 用 -D 传进来，跟着 VERSION 走；单编这个文件时退回 1.0 */
#ifndef TERM_PROGRAM_VERSION_STR
#define TERM_PROGRAM_VERSION_STR "1.0"
#endif

static char g_jb[64];
static int  g_jb_ready = 0;

const char *jb_root(void)
{
    if (!g_jb_ready) {
        g_jb_ready = 1;
        struct stat st;
        if (stat("/var/jb/usr/bin", &st) == 0 && S_ISDIR(st.st_mode))
            snprintf(g_jb, sizeof(g_jb), "/var/jb");
        else
            g_jb[0] = 0;
    }
    return g_jb;
}

void jb_path(char *out, size_t n, const char *sub)
{
    snprintf(out, n, "%s%s", jb_root(), sub);
}

const char *default_home(void)
{
    const char *h = getenv("HOME");
    if (h && h[0]) return h;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir) return pw->pw_dir;
    return getuid() == 0 ? "/var/root" : "/var/mobile";
}

static int is_exec(const char *p)
{
    return p && access(p, X_OK) == 0;
}

const char *default_shell(void)
{
    static char buf[256];
    if (buf[0]) return buf;
    const char *cands[10];
    int n = 0;
    const char *env = getenv("SHELL");
    if (env && env[0]) cands[n++] = env;
    static char zsh_jb[256], bash_jb[256];
    jb_path(zsh_jb, sizeof(zsh_jb), "/bin/zsh");
    jb_path(bash_jb, sizeof(bash_jb), "/bin/bash");
    cands[n++] = zsh_jb;
    cands[n++] = "/bin/zsh";
    cands[n++] = bash_jb;
    cands[n++] = "/bin/bash";
    cands[n++] = "/bin/sh";
    for (int i = 0; i < n; i++) {
        if (is_exec(cands[i])) {
            snprintf(buf, sizeof(buf), "%s", cands[i]);
            return buf;
        }
    }
    snprintf(buf, sizeof(buf), "/bin/sh");
    return buf;
}

const char *build_path(void)
{
    static char buf[1024];
    if (buf[0]) return buf;
    const char *jb = jb_root();
    snprintf(buf, sizeof(buf),
             "%s/usr/local/bin:%s/usr/bin:%s/bin:%s/usr/sbin:%s/sbin:"
             "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
             jb, jb, jb, jb, jb);
    return buf;
}

/* 某个 locale 名字在本机 libc 里是不是真的可用。
   Darwin 上「/usr/share/locale/<name> 目录存在」并不等于可用：C.UTF-8、
   zh_CN.utf8 这类名字在 macOS 与 iOS 上都没有数据，而 libc 反倒认得只有
   LC_CTYPE 一个分类的 "UTF-8"。所以这里直接问 libc：LC_CTYPE 建得出来、
   并且全角字宽度是 2（"中"），才算数。 */
static int locale_ctype_ok(const char *name)
{
    if (!name || !name[0]) return 0;
    locale_t l = newlocale(LC_CTYPE_MASK, name, NULL);
    if (!l) return 0;
    int ok = (wcwidth_l(0x4E2D, l) == 2);
    freelocale(l);
    return ok;
}

/* newlocale(LC_ALL_MASK) 要求六个分类都有数据（"UTF-8" 只有 LC_CTYPE，
   就不满足）。只有满足的名字才能写进 LANG / LC_ALL：否则 shell 里的
   setlocale(LC_ALL, "") 会整体失败，反而退化成单字节模式。 */
static int locale_full_ok(const char *name)
{
    if (!name || !name[0]) return 0;
    locale_t l = newlocale(LC_ALL_MASK, name, NULL);
    if (!l) return 0;
    freelocale(l);
    return 1;
}

int lang_is_full(const char *name)
{
    return locale_full_ok(name);
}

const char *pick_lang(void)
{
    static const char *cands[] = { "zh_CN.UTF-8", "en_US.UTF-8", "UTF-8" };
    static char picked[64];
    if (picked[0]) return picked;
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        if (locale_ctype_ok(cands[i])) {
            snprintf(picked, sizeof(picked), "%s", cands[i]);
            return picked;
        }
    }
    snprintf(picked, sizeof(picked), "%s", "en_US.UTF-8");
    return picked;
}

static void env_push(char **arr, int *n, int max, const char *k, const char *v)
{
    if (*n >= max - 1) return;
    size_t l = strlen(k) + (v ? strlen(v) : 0) + 2;
    char *s = (char *)malloc(l);
    snprintf(s, l, "%s=%s", k, v ? v : "");
    arr[(*n)++] = s;
}

char **build_env(const char *shell, const char *term, const char *lang, int cols, int rows)
{
    char **env = (char **)calloc(48, sizeof(char *));
    int n = 0;
    char tmp[512];
    const char *home = default_home();
    struct passwd *pw = getpwuid(getuid());
    const char *user = (pw && pw->pw_name) ? pw->pw_name : (getuid() == 0 ? "root" : "mobile");

    env_push(env, &n, 48, "PATH", build_path());
    env_push(env, &n, 48, "HOME", home);
    env_push(env, &n, 48, "USER", user);
    env_push(env, &n, 48, "LOGNAME", user);
    env_push(env, &n, 48, "SHELL", shell);
    env_push(env, &n, 48, "TERM", term ? term : "xterm-256color");
    env_push(env, &n, 48, "COLORTERM", "truecolor");
    /* locale：shell 的字符集是不是 UTF-8，直接决定中文能不能正常输入与回显。
       zsh 的 ZLE 在单字节模式下会把中文回显成 <00ad> 这类记法（屏幕上就是乱码），
       bash 的 readline 则把字节原样吐出来，所以同样的坏 locale 只在 zsh 上明显。
       这里把 LANG / LC_CTYPE / LC_ALL 一起设成一个本机真正可用的 UTF-8 locale；
       LC_ALL 优先级最高，用户 rc 里残留的 LANG=C.UTF-8 之类坏值（C.UTF-8 在
       Darwin 上并不存在）也压不住它。 */
    const char *lc = (lang && lang[0]) ? lang : "en_US.UTF-8";
    if (lang_is_full(lc)) {
        env_push(env, &n, 48, "LANG", lc);
        env_push(env, &n, 48, "LC_CTYPE", lc);
        env_push(env, &n, 48, "LC_ALL", lc);
    } else if (locale_ctype_ok(lc)) {
        /* 只有 LC_CTYPE 可用时不能写 LC_ALL，否则 setlocale 会整体失败 */
        env_push(env, &n, 48, "LC_CTYPE", lc);
    } else {
        env_push(env, &n, 48, "LANG", lc);
    }
    env_push(env, &n, 48, "TERM_PROGRAM", "Terminal");
    env_push(env, &n, 48, "TERM_PROGRAM_VERSION", TERM_PROGRAM_VERSION_STR);
    snprintf(tmp, sizeof(tmp), "COLUMNS=%d", cols);
    env_push(env, &n, 48, "COLUMNS", tmp + 8);
    snprintf(tmp, sizeof(tmp), "LINES=%d", rows);
    env_push(env, &n, 48, "LINES", tmp + 6);
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir) env_push(env, &n, 48, "TMPDIR", tmpdir);
    snprintf(tmp, sizeof(tmp), "/tmp");
    struct stat st;
    if (stat(tmp, &st) != 0) jb_path(tmp, sizeof(tmp), "/tmp");
    env_push(env, &n, 48, "JP_ROOT", jb_root());
    env[++n] = NULL;
    return env;
}

void free_env(char **env)
{
    if (!env) return;
    for (int i = 0; env[i]; i++) free(env[i]);
    free(env);
}
