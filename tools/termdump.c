/*
 * 主机端联调工具：真的起一个 shell，把输出喂给终端核心，然后把屏幕 dump 出来。
 * 用法：termdump [-c 列数] [-r 行数] [-n 秒数] [-a] -- 命令...
 *   -a  用 ANSI 颜色输出（方便肉眼看配色）
 */
#include "../src/core/vt.h"
#include "../src/core/vt_unicode.h"
#include "../src/host/pty.h"
#include "../src/host/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>

static Vt *g_vt;
static Pty g_pty;

static void on_write(void *ud, const char *d, size_t n) { (void)ud; pty_write(&g_pty, d, n); }

static void dump_row(Vt *vt, int row, int ansi)
{
    VtLine *l = vt_line_at(vt, row);
    if (!l) { printf("\n"); return; }
    int x = 0;
    while (x < l->cols) {
        VtCell *c = &l->cells[x];
        if (c->width == 0) { x++; continue; }
        if (c->cp == 0) { x++; continue; }
        if (ansi) {
            int fg = -1, bg = -1;
            uint32_t f = c->fg, b = c->bg;
            if (vt->reverse_video ^ ((c->flags & VT_REVERSE) != 0)) { uint32_t t = f; f = b; b = t; }
            if (VT_COLOR_IS_INDEXED(f)) fg = VT_COLOR_INDEX(f);
            else if (VT_COLOR_IS_RGB(f)) fg = 38;
            if (VT_COLOR_IS_INDEXED(b)) bg = VT_COLOR_INDEX(b);
            else if (VT_COLOR_IS_RGB(b)) bg = 48;
            printf("\x1b[0");
            if (c->flags & VT_BOLD) printf(";1");
            if (c->flags & VT_UNDERLINE) printf(";4");
            if (fg == 38) printf(";38;2;%d;%d;%d", VT_COLOR_R(f), VT_COLOR_G(f), VT_COLOR_B(f));
            else if (fg >= 0) printf(";38;5;%d", fg);
            if (bg == 48) printf(";48;2;%d;%d;%d", VT_COLOR_R(b), VT_COLOR_G(b), VT_COLOR_B(b));
            else if (bg >= 0) printf(";48;5;%d", bg);
            printf("m");
        }
        uint8_t u[4];
        int n = vt_u8_encode(c->cp, u);
        fwrite(u, 1, (size_t)n, stdout);
        if (c->cp2) { n = vt_u8_encode(c->cp2, u); fwrite(u, 1, (size_t)n, stdout); }
        if (c->cp3) { n = vt_u8_encode(c->cp3, u); fwrite(u, 1, (size_t)n, stdout); }
        x += c->width ? c->width : 1;
    }
    if (ansi) printf("\x1b[0m");
    printf("\n");
}

int main(int argc, char **argv)
{
    int cols = 60, rows = 12, secs = 3, ansi = 0;
    const char *shell = NULL, *term = "xterm-256color", *lang = NULL;
    char *cmd = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) cols = atoi(argv[++i]);
        else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) rows = atoi(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) secs = atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) shell = argv[++i];
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) term = argv[++i];
        else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) lang = argv[++i];
        else if (strcmp(argv[i], "-a") == 0) ansi = 1;
        else if (strcmp(argv[i], "--") == 0 && i + 1 < argc) { cmd = argv[i + 1]; break; }
    }
    if (!shell) shell = getenv("TERMDUMP_SHELL") ? getenv("TERMDUMP_SHELL") : "/bin/zsh";
    if (!lang) lang = pick_lang();

    g_vt = vt_new(cols, rows, 2000);
    g_vt->ud = g_vt;
    g_vt->write_cb = on_write;
    g_vt->dirty_cb = NULL;

    char **env = build_env(shell, term, lang, cols, rows);
    if (pty_spawn(&g_pty, shell, cols, rows, env) != 0) {
        fprintf(stderr, "forkpty 失败\n");
        return 1;
    }
    free_env(env);

    if (cmd) {
        char buf[4096];
        snprintf(buf, sizeof(buf), "%s\nexit\n", cmd);
        pty_write(&g_pty, buf, strlen(buf));
    }

    struct timeval start, now;
    gettimeofday(&start, NULL);
    char rbuf[65536];
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_pty.fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        int r = select(g_pty.fd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(g_pty.fd, &rfds)) {
            int n = pty_read(&g_pty, rbuf, sizeof(rbuf));
            if (n > 0) vt_input(g_vt, rbuf, (size_t)n);
            else if (n == 0) break;
        }
        gettimeofday(&now, NULL);
        double el = (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_usec - start.tv_usec) / 1e6;
        if (el > secs) break;
    }

    printf("=== 屏幕 %dx%d (光标 %d,%d) %s ===\n", cols, rows, vt_cursor_x(g_vt), vt_cursor_y(g_vt),
           vt_title(g_vt)[0] ? vt_title(g_vt) : "");
    for (int y = 0; y < rows; y++) dump_row(g_vt, vt_sb_offset(g_vt) + y, ansi);
    int sb = vt_sb_offset(g_vt);
    printf("=== 回滚区 %d 行(尾部) ===\n", sb);
    int from = sb > 8 ? sb - 8 : 0;
    for (int y = from; y < sb; y++) {
        char *t = vt_row_text(g_vt, y, true);
        printf("[%d] %s\n", y, t);
        free(t);
    }
    pty_close(&g_pty);
    vt_free(g_vt);
    return 0;
}
