#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>

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

const char *pick_lang(void)
{
    static const char *cands[] = { "zh_CN.UTF-8", "zh_CN.utf8", "en_US.UTF-8",
                                   "en_US.utf8", "C.UTF-8", "UTF-8" };
    struct stat st;
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        char p[256];
        snprintf(p, sizeof(p), "/usr/share/locale/%s", cands[i]);
        if (stat(p, &st) == 0) return cands[i];
        jb_path(p, sizeof(p), "/usr/share/locale");
        char p2[320];
        snprintf(p2, sizeof(p2), "%s/%s", p, cands[i]);
        if (stat(p2, &st) == 0) return cands[i];
    }
    /* iOS 上 locale 目录通常没有，但 libc 认 UTF-8，直接用 */
    return "en_US.UTF-8";
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
    env_push(env, &n, 48, "LANG", lang ? lang : "en_US.UTF-8");
    env_push(env, &n, 48, "TERM_PROGRAM", "Terminal");
    env_push(env, &n, 48, "TERM_PROGRAM_VERSION", "1.0");
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
