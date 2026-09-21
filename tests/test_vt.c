/* 终端核心单元测试：在 macOS 上直接跑，不需要设备 */
#include "../src/core/vt.h"
#include "../src/core/vt_unicode.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_checks = 0, g_fail = 0;
#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("  FAIL %d: %s\n", __LINE__, #cond); } \
} while (0)

static void check_str(int line, const char *got, const char *want)
{
    g_checks++;
    if (strcmp(got, want) != 0) {
        g_fail++;
        printf("  FAIL %d: got \"%s\" want \"%s\"\n", line, got, want);
    }
}
#define CHECK_STR(g, w) check_str(__LINE__, (g), (w))
#define CHECK_INT(g, w) do { \
    g_checks++; \
    long _g = (long)(g), _w = (long)(w); \
    if (_g != _w) { g_fail++; printf("  FAIL %d: got %ld want %ld\n", __LINE__, _g, _w); } \
} while (0)

typedef struct { char buf[8192]; size_t len; int bells; char title[256]; char clip[512]; } Cap;

static void cap_write(void *ud, const char *d, size_t n)
{
    Cap *c = (Cap *)ud;
    if (c->len + n < sizeof(c->buf)) {
        memcpy(c->buf + c->len, d, n);
        c->len += n;
        c->buf[c->len] = 0;
    }
}
static void cap_bell(void *ud) { ((Cap *)ud)->bells++; }
static void cap_title(void *ud, const char *t)
{
    Cap *c = (Cap *)ud;
    snprintf(c->title, sizeof(c->title), "%s", t);
}
static void cap_clip(void *ud, const char *d, size_t n)
{
    Cap *c = (Cap *)ud;
    size_t k = n < sizeof(c->clip) - 1 ? n : sizeof(c->clip) - 1;
    memcpy(c->clip, d, k);
    c->clip[k] = 0;
}

typedef struct { Vt *vt; Cap cap; } Fix;

static void fix_init(Fix *f, int cols, int rows, int sb)
{
    memset(f, 0, sizeof(*f));
    f->vt = vt_new(cols, rows, sb);
    f->vt->ud = &f->cap;
    f->vt->write_cb = cap_write;
    f->vt->bell_cb = cap_bell;
    f->vt->title_cb = cap_title;
    f->vt->clipboard_cb = cap_clip;
}
static void fix_free(Fix *f) { vt_free(f->vt); }
static void feed(Fix *f, const char *s) { vt_input(f->vt, s, strlen(s)); }

static void row(Fix *f, int y, char *out, size_t n)
{
    char *t = vt_row_text(f->vt, vt_sb_offset(f->vt) + y, true);
    snprintf(out, n, "%s", t);
    free(t);
}

/* ---------------------------------------------------------------- */
static void test_utf8(void)
{
    printf("utf8\n");
    uint32_t cp = 0;
    const uint8_t zh[] = { 0xE4, 0xB8, 0xAD };          /* 中 */
    CHECK_INT(vt_u8_decode(zh, 3, &cp), 3);
    CHECK_INT(cp, 0x4E2D);
    uint8_t out[4];
    CHECK_INT(vt_u8_encode(0x4E2D, out), 3);
    CHECK(memcmp(out, zh, 3) == 0);
    CHECK_INT(vt_u8_len(0x1F600), 4);
    /* 残缺序列 -> U+FFFD 且不会吞掉后面的字符 */
    const uint8_t bad[] = { 0xE4, 0x41 };
    CHECK_INT(vt_u8_decode(bad, 2, &cp), 1);
    CHECK_INT(cp, VT_REPLACEMENT_CHAR);
    CHECK_INT(vt_u8_decode(bad + 1, 1, &cp), 1);
    CHECK_INT(cp, 'A');
    /* 非法起始字节 */
    const uint8_t bad2[] = { 0x80 };
    CHECK_INT(vt_u8_decode(bad2, 1, &cp), 1);
    CHECK_INT(cp, VT_REPLACEMENT_CHAR);
}

static void test_width(void)
{
    printf("width\n");
    CHECK_INT(vt_cell_width('a'), 1);
    CHECK_INT(vt_cell_width(0x4E2D), 2);      /* 中 */
    CHECK_INT(vt_cell_width(0x3002), 2);      /* 。 */
    CHECK_INT(vt_cell_width(0xFF01), 2);      /* ！全角 */
    CHECK_INT(vt_cell_width(0x0301), 0);      /* 组合重音 */
    CHECK_INT(vt_cell_width(0x200D), 0);      /* ZWJ */
    CHECK_INT(vt_cell_width(0xFE0F), 0);      /* 变体选择符 */
    CHECK_INT(vt_cell_width(0x1F600), 2);     /* emoji */
    CHECK_INT(vt_cell_width(0xAC00), 2);      /* 韩文 */
    CHECK_INT(vt_cell_width(0x3042), 2);      /* あ */
    CHECK_INT(vt_cell_width('!'), 1);
}

static void test_basic_print(void)
{
    printf("basic print\n");
    Fix f;
    fix_init(&f, 20, 5, 10);
    feed(&f, "hello");
    CHECK_INT(vt_cursor_x(f.vt), 5);
    CHECK_INT(vt_cursor_y(f.vt), 0);
    char r[64];
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "hello");
    feed(&f, "\r\nworld");
    row(&f, 1, r, sizeof(r));
    CHECK_STR(r, "world");
    fix_free(&f);
}

/* 中文按 2 列算，列对齐才不会乱 —— 这是最关键的一条 */
static void test_cjk_alignment(void)
{
    printf("cjk alignment\n");
    Fix f;
    fix_init(&f, 40, 5, 10);
    feed(&f, "中文名.txt   1024");
    /* 中文名 = 3 个汉字 = 6 列 */
    CHECK_INT(vt_cursor_x(f.vt), 6 + 4 + 3 + 4);
    char r[128];
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "中文名.txt   1024");

    /* 光标落在宽字符的第二格时要回到字符开头 */
    fix_free(&f);
    fix_init(&f, 40, 5, 10);
    feed(&f, "中文");
    CHECK_INT(vt_cursor_x(f.vt), 4);
    feed(&f, "\x1b[D");                 /* 左移一格 -> 落在"文"的第二格 */
    CHECK_INT(vt_cursor_x(f.vt), 3);
    feed(&f, "\x1b[D");                 /* 再左移 -> "文"的第一格 */
    CHECK_INT(vt_cursor_x(f.vt), 2);
    feed(&f, "\x1b[D\x1b[D");          /* 回到行首 */
    CHECK_INT(vt_cursor_x(f.vt), 0);
    /* 覆盖宽字符的一半时，整个字要清掉 */
    feed(&f, "\x1b[2GX");
    CHECK_INT(vt_screen_line(f.vt, 0)->cells[0].cp, ' ');
    CHECK_INT(vt_screen_line(f.vt, 0)->cells[1].cp, 'X');
    CHECK_INT(vt_screen_line(f.vt, 0)->cells[2].cp, 0x6587);   /* 文 不受影响 */
    fix_free(&f);
}

static void test_wide_wrap(void)
{
    printf("wide wrap\n");
    Fix f;
    fix_init(&f, 5, 4, 10);
    feed(&f, "ab中");
    CHECK_INT(vt_cursor_x(f.vt), 4);
    /* 第 4 列放不下宽字符(需要 4,5)，应该折到下一行 */
    feed(&f, "中");
    CHECK_INT(vt_cursor_y(f.vt), 1);
    CHECK_INT(vt_cursor_x(f.vt), 2);
    char r[64];
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "ab中");
    row(&f, 1, r, sizeof(r));
    CHECK_STR(r, "中");
    fix_free(&f);
}

static void test_pending_wrap(void)
{
    printf("pending wrap (DECAWM)\n");
    Fix f;
    fix_init(&f, 4, 4, 10);
    feed(&f, "abcd");
    /* 第 4 个字符写在第 4 列，光标停在原地(延迟折行) */
    CHECK_INT(vt_cursor_x(f.vt), 3);
    CHECK_INT(vt_cursor_y(f.vt), 0);
    feed(&f, "e");
    CHECK_INT(vt_cursor_y(f.vt), 1);
    CHECK_INT(vt_cursor_x(f.vt), 1);
    char r[64];
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "abcd");
    row(&f, 1, r, sizeof(r));
    CHECK_STR(r, "e");
    /* 关闭自动折行后应该覆盖最后一列 */
    fix_free(&f);
    fix_init(&f, 4, 4, 10);
    feed(&f, "\x1b[?7l");
    feed(&f, "abcdef");
    CHECK_INT(vt_cursor_y(f.vt), 0);
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "abcf");
    fix_free(&f);
}

static void test_scrollback(void)
{
    printf("scrollback\n");
    Fix f;
    fix_init(&f, 10, 3, 100);
    for (int i = 0; i < 10; i++) {
        char b[32];
        snprintf(b, sizeof(b), "line%d\r\n", i);
        feed(&f, b);
    }
    CHECK_INT(vt_sb_offset(f.vt), 8);
    char *t = vt_row_text(f.vt, 0, true);
    CHECK_STR(t, "line0");
    free(t);
    t = vt_row_text(f.vt, 7, true);
    CHECK_STR(t, "line7");
    free(t);
    char r[32];
    row(&f, 1, r, sizeof(r));
    CHECK_STR(r, "line9");
    row(&f, 2, r, sizeof(r));
    CHECK_STR(r, "");
    /* 回滚区满了以后丢最老的 */
    fix_free(&f);
    fix_init(&f, 10, 3, 4);
    for (int i = 0; i < 10; i++) {
        char b[32];
        snprintf(b, sizeof(b), "l%d\r\n", i);
        feed(&f, b);
    }
    CHECK_INT(vt_sb_offset(f.vt), 4);
    t = vt_row_text(f.vt, 0, true);
    CHECK_STR(t, "l4");
    free(t);
    fix_free(&f);
}

static void test_sgr(void)
{
    printf("sgr\n");
    Fix f;
    fix_init(&f, 20, 4, 10);
    feed(&f, "\x1b[1;31mX");
    VtLine *l = vt_screen_line(f.vt, 0);
    CHECK_INT(l->cells[0].flags & VT_BOLD, VT_BOLD);
    CHECK_INT(VT_COLOR_INDEX(l->cells[0].fg), 1);
    feed(&f, "\x1b[0mY");
    CHECK_INT(l->cells[1].flags, 0);
    CHECK_INT(l->cells[1].fg, VT_COLOR_DEFAULT);
    /* 256 色 */
    feed(&f, "\x1b[38;5;208mP");
    CHECK_INT(VT_COLOR_IS_INDEXED(l->cells[2].fg), 1);
    CHECK_INT(VT_COLOR_INDEX(l->cells[2].fg), 208);
    /* 真彩色，分号写法 */
    feed(&f, "\x1b[48;2;10;20;30mQ");
    CHECK_INT(VT_COLOR_IS_RGB(l->cells[3].bg), 1);
    CHECK_INT(VT_COLOR_R(l->cells[3].bg), 10);
    CHECK_INT(VT_COLOR_G(l->cells[3].bg), 20);
    CHECK_INT(VT_COLOR_B(l->cells[3].bg), 30);
    /* 冒号写法 + 颜色空间空槽 */
    feed(&f, "\x1b[38:2::255:128:64mR");
    CHECK_INT(VT_COLOR_R(l->cells[4].fg), 255);
    CHECK_INT(VT_COLOR_G(l->cells[4].fg), 128);
    CHECK_INT(VT_COLOR_B(l->cells[4].fg), 64);
    /* 亮色 + 反显 */
    feed(&f, "\x1b[7;97mS");
    CHECK_INT(l->cells[5].flags & VT_REVERSE, VT_REVERSE);
    CHECK_INT(VT_COLOR_INDEX(l->cells[5].fg), 15);
    feed(&f, "\x1b[mT");
    CHECK_INT(l->cells[6].flags, 0);
    fix_free(&f);
}

static void test_cursor_ops(void)
{
    printf("cursor ops\n");
    Fix f;
    fix_init(&f, 20, 6, 10);
    feed(&f, "\x1b[3;5H");
    CHECK_INT(vt_cursor_y(f.vt), 2);
    CHECK_INT(vt_cursor_x(f.vt), 4);
    feed(&f, "\x1b[2A");
    CHECK_INT(vt_cursor_y(f.vt), 0);
    feed(&f, "\x1b[3B\x1b[2C");
    CHECK_INT(vt_cursor_y(f.vt), 3);
    CHECK_INT(vt_cursor_x(f.vt), 6);
    feed(&f, "\x1b[10D");
    CHECK_INT(vt_cursor_x(f.vt), 0);
    feed(&f, "\x1b[5G");
    CHECK_INT(vt_cursor_x(f.vt), 4);
    feed(&f, "\x1b[2d");
    CHECK_INT(vt_cursor_y(f.vt), 1);
    /* 保存/恢复光标 */
    feed(&f, "\x1b[4;7H\x1b" "7\x1b[1;1H\x1b" "8");
    CHECK_INT(vt_cursor_y(f.vt), 3);
    CHECK_INT(vt_cursor_x(f.vt), 6);
    /* 原点模式 */
    feed(&f, "\x1b[2;4r");          /* 滚动区 2..4 */
    feed(&f, "\x1b[?6h");
    feed(&f, "\x1b[1;1H");
    CHECK_INT(vt_cursor_y(f.vt), 1);
    feed(&f, "\x1b[?6l");
    fix_free(&f);
}

static void test_erase(void)
{
    printf("erase\n");
    Fix f;
    fix_init(&f, 10, 4, 10);
    feed(&f, "0123456789\r\nabcdefghij\r\nKLMNOPQRST");
    feed(&f, "\x1b[1;3H\x1b[K");        /* 擦到行尾 */
    char r[32];
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "01");
    feed(&f, "\x1b[2;1H\x1b[1K");       /* 擦到光标处(含) */
    row(&f, 1, r, sizeof(r));
    CHECK_STR(r, " bcdefghij");
    feed(&f, "\x1b[3;1H\x1b[0J");       /* 擦到屏幕尾 */
    row(&f, 2, r, sizeof(r));
    CHECK_STR(r, "");
    feed(&f, "\x1b[2J");
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "");
    fix_free(&f);
}

static void test_insert_delete(void)
{
    printf("insert/delete\n");
    Fix f;
    fix_init(&f, 8, 3, 10);
    feed(&f, "abcdefgh");
    feed(&f, "\x1b[1;3H\x1b[2P");       /* 删 2 个字符 */
    char r[32];
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "abefgh");
    feed(&f, "\x1b[1;1H\x1b[2@");       /* 插 2 个空位 */
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "  abefgh");
    /* 插入模式 */
    feed(&f, "\x1b[1;1H\x1b[4hXY");
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "XY  abef");
    feed(&f, "\x1b[4l");
    /* 插行 / 删行 */
    fix_free(&f);
    fix_init(&f, 8, 4, 10);
    feed(&f, "aaa\r\nbbb\r\nccc\r\nddd");
    feed(&f, "\x1b[2;1H\x1b[L");        /* 在第 2 行插入空行 */
    row(&f, 1, r, sizeof(r));
    CHECK_STR(r, "");
    row(&f, 2, r, sizeof(r));
    CHECK_STR(r, "bbb");
    row(&f, 3, r, sizeof(r));
    CHECK_STR(r, "ccc");
    feed(&f, "\x1b[2;1H\x1b[M");        /* 删掉空行 */
    row(&f, 1, r, sizeof(r));
    CHECK_STR(r, "bbb");
    fix_free(&f);
}

static void test_alt_screen(void)
{
    printf("alt screen\n");
    Fix f;
    fix_init(&f, 10, 3, 10);
    feed(&f, "main1\r\nmain2\r\nmain3");
    feed(&f, "\x1b[?1049h");
    char r[32];
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "");
    feed(&f, "alt!");
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "alt!");
    feed(&f, "\x1b[?1049l");
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "main1");
    CHECK_INT(vt_sb_offset(f.vt), 0);
    fix_free(&f);
}

static void test_scroll_region(void)
{
    printf("scroll region\n");
    Fix f;
    fix_init(&f, 6, 5, 10);
    feed(&f, "111111\r\n222222\r\n333333\r\n444444\r\n555555");
    feed(&f, "\x1b[2;4r");              /* 只滚动 2..4 行 */
    feed(&f, "\x1b[4;1H");
    feed(&f, "\r\n");
    char r[32];
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "111111");
    row(&f, 1, r, sizeof(r));
    CHECK_STR(r, "333333");
    row(&f, 2, r, sizeof(r));
    CHECK_STR(r, "444444");
    row(&f, 3, r, sizeof(r));
    CHECK_STR(r, "");
    row(&f, 4, r, sizeof(r));
    CHECK_STR(r, "555555");
    /* 反向索引 */
    feed(&f, "\x1b[2;1H\x1bM");
    row(&f, 1, r, sizeof(r));
    CHECK_STR(r, "");
    row(&f, 2, r, sizeof(r));
    CHECK_STR(r, "333333");
    fix_free(&f);
}

static void test_osc(void)
{
    printf("osc\n");
    Fix f;
    fix_init(&f, 20, 3, 10);
    feed(&f, "\x1b]0;我的标题\x07");
    CHECK_STR(f.cap.title, "我的标题");
    CHECK_STR(vt_title(f.vt), "我的标题");
    feed(&f, "\x1b]2;another\x1b\\");
    CHECK_STR(f.cap.title, "another");
    /* 调色板设置 */
    feed(&f, "\x1b]4;1;#ff0000\x1b\\");
    CHECK_INT(f.vt->palette[1] & 0xFFFFFFu, 0xFF0000u);
    CHECK(f.vt->palette_custom[1]);
    /* 剪贴板 OSC52 */
    feed(&f, "\x1b]52;c;aGVsbG8=\x1b\\");
    CHECK_STR(f.cap.clip, "hello");
    fix_free(&f);
}

static void test_modes_and_reports(void)
{
    printf("modes / reports\n");
    Fix f;
    fix_init(&f, 20, 5, 10);
    CHECK_INT(f.vt->autowrap, 1);
    feed(&f, "\x1b[?7l");
    CHECK_INT(f.vt->autowrap, 0);
    feed(&f, "\x1b[?7h");
    feed(&f, "\x1b[?2004h");
    CHECK_INT(f.vt->bracketed_paste, 1);
    feed(&f, "\x1b[?25l");
    CHECK_INT(f.vt->cursor_visible, 0);
    /* 光标位置上报 */
    f.cap.len = 0; f.cap.buf[0] = 0;
    feed(&f, "\x1b[3;4H\x1b[6n");
    CHECK_STR(f.cap.buf, "\x1b[3;4R");
    /* 设备属性 */
    f.cap.len = 0; f.cap.buf[0] = 0;
    feed(&f, "\x1b[c");
    CHECK(strstr(f.cap.buf, "?62;") != NULL);
    /* 窗口大小上报 */
    f.cap.len = 0; f.cap.buf[0] = 0;
    f.vt->pixel_w = 640; f.vt->pixel_h = 960;
    feed(&f, "\x1b[18t");
    CHECK_STR(f.cap.buf, "\x1b[8;5;20t");
    f.cap.len = 0; f.cap.buf[0] = 0;
    feed(&f, "\x1b[14t");
    CHECK_STR(f.cap.buf, "\x1b[4;960;640t");
    /* DECRQM：2004 的当前状态 */
    f.cap.len = 0; f.cap.buf[0] = 0;
    feed(&f, "\x1b[?2004$p");
    CHECK_STR(f.cap.buf, "\x1b[?2004;1$y");
    /* 响铃 */
    feed(&f, "\x07");
    CHECK_INT(f.cap.bells, 1);
    fix_free(&f);
}

static void test_charset(void)
{
    printf("charset / DEC graphics\n");
    Fix f;
    fix_init(&f, 10, 3, 10);
    feed(&f, "\x1b(0lqk\x1b(B");
    char r[32];
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "┌─┐");
    fix_free(&f);
}

static void test_combining(void)
{
    printf("combining marks / emoji cluster\n");
    Fix f;
    fix_init(&f, 10, 3, 10);
    feed(&f, "e\xcc\x81x");             /* e + U+0301 */
    CHECK_INT(vt_cursor_x(f.vt), 2);
    VtCell *c = &vt_screen_line(f.vt, 0)->cells[0];
    CHECK_INT(c->cp2, 0x0301);
    /* ZWJ 序列合并进前一格，不额外占位 */
    fix_free(&f);
    fix_init(&f, 10, 3, 10);
    feed(&f, "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9");  /* 👨ZWJ👩 */
    CHECK_INT(vt_cursor_x(f.vt), 2);
    c = &vt_screen_line(f.vt, 0)->cells[0];
    CHECK_INT(c->cp, 0x1F468);
    CHECK_INT(c->cp2, 0x200D);
    CHECK_INT(c->cp3, 0x1F469);
    fix_free(&f);
}

static void test_reflow(void)
{
    printf("resize reflow\n");
    Fix f;
    fix_init(&f, 20, 5, 100);
    feed(&f, "0123456789ABCDEFGHIJ");   /* 刚好 20 列 */
    feed(&f, "0123456789");             /* 折行 */
    vt_resize(f.vt, 10, 5);
    char r[64];
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "0123456789");
    row(&f, 1, r, sizeof(r));
    CHECK_STR(r, "ABCDEFGHIJ");
    row(&f, 2, r, sizeof(r));
    CHECK_STR(r, "0123456789");
    CHECK_INT(vt_cursor_x(f.vt), 9);      /* 停在最后一列，等下一个字符再折行 */
    CHECK_INT(vt_cursor_y(f.vt), 2);
    /* 变宽后应该收回一行 */
    vt_resize(f.vt, 20, 5);
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "0123456789ABCDEFGHIJ");
    row(&f, 1, r, sizeof(r));
    CHECK_STR(r, "0123456789");
    CHECK_INT(vt_cursor_y(f.vt), 1);
    CHECK_INT(vt_cursor_x(f.vt), 10);
    fix_free(&f);

    /* 中文折行 */
    fix_init(&f, 6, 4, 100);
    feed(&f, "中文中文中文");
    vt_resize(f.vt, 4, 4);
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "中文");
    row(&f, 1, r, sizeof(r));
    CHECK_STR(r, "中文");
    row(&f, 2, r, sizeof(r));
    CHECK_STR(r, "中文");
    fix_free(&f);

    /* 键盘弹出/收起只改行数，不能把两行并成一行。
       宽字符结尾的行 + 后面跟一行，是以前踩过的坑。 */
    char big[128];
    fix_init(&f, 40, 8, 100);
    feed(&f, "prompt$ echo 中文和 ASCII 混排\r\n");
    feed(&f, "root@iphone:~#");
    vt_resize(f.vt, 40, 6);
    row(&f, 0, big, sizeof(big));
    CHECK_STR(big, "prompt$ echo 中文和 ASCII 混排");
    row(&f, 1, big, sizeof(big));
    CHECK_STR(big, "root@iphone:~#");
    vt_resize(f.vt, 40, 8);
    row(&f, 0, big, sizeof(big));
    CHECK_STR(big, "prompt$ echo 中文和 ASCII 混排");
    row(&f, 1, big, sizeof(big));
    CHECK_STR(big, "root@iphone:~#");
    fix_free(&f);

    /* 列数变窄再变回来，内容必须一字不差：既不能丢字，也不能凭空多出空格 */
    fix_init(&f, 60, 8, 100);
    feed(&f, "prompt$ echo 中文和 ASCII 混排 mixed content 12345 结束\r\n");
    feed(&f, "root@iphone:~#");
    vt_resize(f.vt, 30, 8);
    vt_resize(f.vt, 60, 8);
    row(&f, 0, big, sizeof(big));
    CHECK_STR(big, "prompt$ echo 中文和 ASCII 混排 mixed content 12345 结束");
    row(&f, 1, big, sizeof(big));
    CHECK_STR(big, "root@iphone:~#");
    fix_free(&f);

    /* 宽字符在行尾放不下时让出的填充格，重新折行时不能变成真空格 */
    fix_init(&f, 9, 4, 100);
    feed(&f, "abcdefgh");      /* 占满前 8 列 */
    feed(&f, "中");            /* 第 9 列放不下宽字符，整体挤到下一行 */
    feed(&f, "\r\nnext");
    vt_resize(f.vt, 12, 4);
    row(&f, 0, big, sizeof(big));
    CHECK_STR(big, "abcdefgh中");
    row(&f, 1, big, sizeof(big));
    CHECK_STR(big, "next");
    fix_free(&f);
}

/* ---------------------------------------------------------------- */
/* 重排引擎的随机往返测试：任意宽度来回缩放，屏幕内容必须一字不差 */
static unsigned g_rnd = 20260918u;

static int rnd_below(int n)
{
    g_rnd = g_rnd * 1103515245u + 12345u;
    return (int)((g_rnd >> 16) % (unsigned)n);
}

static char *screen_text(Vt *vt)
{
    size_t cap = 1024, len = 0;
    char *out = (char *)malloc(cap);
    out[0] = 0;
    int total = vt_total_lines(vt);
    for (int r = 0; r < total; r++) {
        VtLine *l = vt_line_at(vt, r);
        char *t = vt_row_text(vt, r, true);
        size_t n = strlen(t);
        while (len + n + 2 > cap) { cap *= 2; out = (char *)realloc(out, cap); }
        if (r > 0 && !(l->flags & VT_LINE_WRAPPED)) out[len++] = '\n';
        memcpy(out + len, t, n);
        len += n;
        out[len] = 0;
        free(t);
    }
    return out;
}

static void test_reflow_fuzz(void)
{
    printf("resize reflow round-trip\n");
    static const char *const words[] = {
        "echo", "ls -la", "中文测试", "混排 mixed 123", "结束。",
        "printf", "abc", "中", "aa中bb", "，", "0123456789", "~/Desktop",
    };
    const int nwords = (int)(sizeof(words) / sizeof(words[0]));
    int bad = 0;
    for (int iter = 0; iter < 60; iter++) {
        int cols = 12 + rnd_below(30);
        Fix f;
        fix_init(&f, cols, 8, 4000);
        char line[512];
        int nlines = 1 + rnd_below(10);
        for (int i = 0; i < nlines; i++) {
            line[0] = 0;
            int nw = 1 + rnd_below(4);
            for (int k = 0; k < nw; k++) {
                if (k) strcat(line, " ");
                strcat(line, words[rnd_below(nwords)]);
            }
            strcat(line, "\r\n");
            feed(&f, line);
        }
        char *want = screen_text(f.vt);
        vt_resize(f.vt, 8 + rnd_below(50), 4 + rnd_below(12));
        vt_resize(f.vt, cols, 8);
        char *got = screen_text(f.vt);
        if (strcmp(want, got) != 0) {
            bad++;
            if (bad == 1) {
                printf("  FAIL %d: %d 列往返还原不一致\n", __LINE__, cols);
                printf("    want: %s\n", want);
                printf("    got : %s\n", got);
            }
        }
        free(want);
        free(got);
        fix_free(&f);
    }
    g_checks++;
    if (bad) g_fail++;
}

static void test_text_extract(void)
{
    printf("text extraction\n");
    Fix f;
    fix_init(&f, 20, 5, 100);
    feed(&f, "abc中文\r\ndef\r\nghi");
    char *t = vt_range_text(f.vt, 0, 0, 2, 2);
    CHECK_STR(t, "abc中文\ndef\nghi");
    free(t);
    /* 折行不该插换行符 */
    fix_free(&f);
    fix_init(&f, 5, 4, 100);
    feed(&f, "abcdefgh");
    t = vt_range_text(f.vt, 0, 0, 1, 2);
    CHECK_STR(t, "abcdefgh");
    free(t);
    fix_free(&f);
}

static void test_keys(void)
{
    printf("key encoding\n");
    Fix f;
    fix_init(&f, 20, 5, 10);
    char b[32];
    CHECK_INT(vt_key_encode(f.vt, VK_UP, 0, 0, b, sizeof(b)), 3);
    CHECK_STR(b, "\x1b[A");
    f.vt->appcursor = true;
    vt_key_encode(f.vt, VK_UP, 0, 0, b, sizeof(b));
    CHECK_STR(b, "\x1bOA");
    f.vt->appcursor = false;
    vt_key_encode(f.vt, VK_UP, VTM_CTRL, 0, b, sizeof(b));
    CHECK_STR(b, "\x1b[1;5A");
    vt_key_encode(f.vt, VK_CHAR, VTM_CTRL, 'c', b, sizeof(b));
    CHECK_STR(b, "\x03");
    vt_key_encode(f.vt, VK_CHAR, VTM_CTRL, 'd', b, sizeof(b));
    CHECK_STR(b, "\x04");
    vt_key_encode(f.vt, VK_CHAR, VTM_ALT, 'x', b, sizeof(b));
    CHECK_STR(b, "\x1bx");
    vt_key_encode(f.vt, VK_CHAR, 0, 0x4E2D, b, sizeof(b));
    CHECK_STR(b, "中");
    vt_key_encode(f.vt, VK_DELETE, 0, 0, b, sizeof(b));
    CHECK_STR(b, "\x1b[3~");
    vt_key_encode(f.vt, VK_HOME, 0, 0, b, sizeof(b));
    CHECK_STR(b, "\x1b[H");
    vt_key_encode(f.vt, VK_ENTER, 0, 0, b, sizeof(b));
    CHECK_STR(b, "\r");
    vt_key_encode(f.vt, VK_BACKSPACE, 0, 0, b, sizeof(b));
    CHECK_STR(b, "\x7f");
    vt_key_encode(f.vt, VK_PAGEUP, VTM_SHIFT, 0, b, sizeof(b));
    CHECK_STR(b, "\x1b[5;2~");
    CHECK_INT(vt_key_encode(f.vt, VK_TAB, 0, 0, b, sizeof(b)), 1);
    CHECK_STR(b, "\t");
    vt_key_encode(f.vt, VK_TAB, VTM_SHIFT, 0, b, sizeof(b));
    CHECK_STR(b, "\x1b[Z");
    vt_key_encode(f.vt, VK_BACKTAB, 0, 0, b, sizeof(b));
    CHECK_STR(b, "\x1b[Z");
    fix_free(&f);
}

static void test_rep_and_dsr_origin(void)
{
    printf("misc\n");
    Fix f;
    fix_init(&f, 10, 4, 10);
    feed(&f, "a\x1b[3b");
    char r[32];
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "aaaa");
    /* 制表符 */
    fix_free(&f);
    fix_init(&f, 16, 4, 10);
    feed(&f, "ab\tc");
    CHECK_INT(vt_cursor_x(f.vt), 9);
    feed(&f, "\x1b[Z");
    CHECK_INT(vt_cursor_x(f.vt), 8);
    feed(&f, "\x1b[99C");
    CHECK_INT(vt_cursor_x(f.vt), 15);
    /* tmux 透传 */
    fix_free(&f);
    fix_init(&f, 20, 4, 10);
    feed(&f, "\x1bPtmux;\x1b\x1b]0;from tmux\x07\x1b\\");
    CHECK_STR(f.cap.title, "from tmux");
    /* RIS */
    feed(&f, "\x1b" "c");
    CHECK_INT(vt_cursor_x(f.vt), 0);
    CHECK_INT(f.vt->autowrap, 1);
    fix_free(&f);
}

static void test_bracketed_paste_roundtrip(void)
{
    printf("bracketed paste + utf8 split across chunks\n");
    Fix f;
    fix_init(&f, 20, 4, 10);
    /* 一个汉字被拆成两次喂进来 */
    const char zh[] = { 0xE4, 0xB8, 0xAD };
    vt_input(f.vt, zh, 1);
    vt_input(f.vt, zh + 1, 2);
    char r[32];
    row(&f, 0, r, sizeof(r));
    CHECK_STR(r, "中");
    CHECK_INT(vt_cursor_x(f.vt), 2);
    /* 无效字节不应该吞掉后面的内容 */
    vt_input(f.vt, "\x80\x81", 2);
    feed(&f, "ok");
    row(&f, 0, r, sizeof(r));
    CHECK(strstr(r, "ok") != NULL);
    fix_free(&f);
}

int main(void)
{
    test_utf8();
    test_width();
    test_basic_print();
    test_cjk_alignment();
    test_wide_wrap();
    test_pending_wrap();
    test_scrollback();
    test_sgr();
    test_cursor_ops();
    test_erase();
    test_insert_delete();
    test_alt_screen();
    test_scroll_region();
    test_osc();
    test_modes_and_reports();
    test_charset();
    test_combining();
    test_reflow();
    test_reflow_fuzz();
    test_text_extract();
    test_keys();
    test_rep_and_dsr_origin();
    test_bracketed_paste_roundtrip();
    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
