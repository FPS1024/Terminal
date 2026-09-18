/*
 * vt.h - 终端模拟核心 (VT100/VT220 + xterm 扩展)
 *
 * 设计要点：
 *  - 每个 cell 最多存 3 个码点(基字符 + 组合字符/ZWJ 序列)，中文/emoji 都不散架
 *  - 屏幕按行存，滚动只是行指针搬家，回滚区是环形缓冲，`yes` 刷屏也不卡
 *  - 尺寸变化时按逻辑行重新折行(reflow)，横竖屏切换不会把历史打乱
 *  - 所有响应(CPR/DA/DECRQSS/剪贴板)通过 write_cb 回写 pty
 */
#ifndef VT_H
#define VT_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 颜色 ---------------- */
#define VT_COLOR_DEFAULT        0xFFFFFFFFu
#define VT_COLOR_IDX(i)         (0x01000000u | ((uint32_t)(i) & 0xFFu))
#define VT_COLOR_RGB(r,g,b)     (0x02000000u | (((uint32_t)(r) & 0xFFu) << 16) | \
                                 (((uint32_t)(g) & 0xFFu) << 8) | ((uint32_t)(b) & 0xFFu))
#define VT_COLOR_IS_INDEXED(c)  (((c) >> 24) == 0x01u)
#define VT_COLOR_IS_RGB(c)      (((c) >> 24) == 0x02u)
#define VT_COLOR_INDEX(c)       ((int)((c) & 0xFFu))
#define VT_COLOR_R(c)           ((int)(((c) >> 16) & 0xFFu))
#define VT_COLOR_G(c)           ((int)(((c) >> 8) & 0xFFu))
#define VT_COLOR_B(c)           ((int)((c) & 0xFFu))

/* ---------------- 属性位 ---------------- */
enum {
    VT_BOLD        = 1 << 0,
    VT_DIM         = 1 << 1,
    VT_ITALIC      = 1 << 2,
    VT_UNDERLINE   = 1 << 3,
    VT_BLINK       = 1 << 4,
    VT_REVERSE     = 1 << 5,
    VT_INVISIBLE   = 1 << 6,
    VT_STRIKE      = 1 << 7,
    VT_DBLUNDER    = 1 << 8,
    VT_WIDE_CONT   = 1 << 9,   /* 宽字符的第二格，渲染时跳过 */
    VT_WIDE_PAD    = 1 << 10,  /* 宽字符折行时让出的填充格，重新折行时丢弃 */
};

typedef struct {
    uint32_t cp;      /* 基字符，0 表示空 */
    uint32_t cp2;     /* 组合字符 / 零宽 */
    uint32_t cp3;     /* 再一个组合字符 / ZWJ 后面的 emoji */
    uint32_t fg;
    uint32_t bg;
    uint16_t flags;
    uint8_t  width;   /* 占几列：1 或 2 */
    uint8_t  reserved;
} VtCell;             /* 24 字节 */

typedef struct {
    VtCell  *cells;   /* cols 个，本行自己拥有 */
    uint16_t cols;
    uint16_t flags;
    uint32_t rev;     /* 渲染缓存用的版本号 */
} VtLine;

#define VT_LINE_WRAPPED 1u   /* 这行是折行折出来的，下一行是它的续行 */
#define VT_LINE_PROMPT  2u   /* OSC 133 标记：提示符所在行 */
#define VT_LINE_KEEP    4u   /* 不允许被回滚区回收(视图正在引用) */

/* ---------------- 回调 ---------------- */
typedef void (*VtWriteFn)(void *ud, const char *data, size_t len);
typedef void (*VtBellFn)(void *ud);
typedef void (*VtTitleFn)(void *ud, const char *title);
typedef void (*VtClipboardFn)(void *ud, const char *data, size_t len);  /* OSC52 写剪贴板 */
typedef void (*VtNotifyFn)(void *ud, const char *title, const char *body);
typedef void (*VtDirtyFn)(void *ud);
typedef void (*VtPromptFn)(void *ud, int row);   /* OSC133: 新提示符在第几行 */

/* ---------------- 主结构 ---------------- */
typedef struct {
    int      cols, rows;
    VtLine  *lines;          /* rows 行 */
    int      cx, cy;
    int      scroll_top, scroll_bot;   /* 闭区间 */
    int      saved_cx, saved_cy;
    uint32_t saved_fg, saved_bg;
    uint16_t saved_flags;
    int      saved_charset[2];
    int      wrapped;        /* 上一行是否折行 */
} VtBuffer;

typedef struct Vt {
    VtBuffer normal, alt;
    VtBuffer *buf;
    bool     alt_active;
    int      cols, rows;
    /* 当前 SGR 状态 */
    uint32_t cur_fg, cur_bg;
    uint16_t cur_flags;
    /* 模式 */
    bool autowrap, origin, insert, appcursor, appkeypad;
    bool cursor_visible, reverse_video, bracketed_paste, focus_events;
    bool reverse_wrap, alt_scroll_mode, smooth_scroll, newline_mode;
    int  cursor_style;         /* DECSCUSR: 0..6 */
    int  mouse_mode;           /* 0=off, 9=X10, 1000, 1002, 1003 */
    bool mouse_sgr, mouse_urxvt, mouse_utf8;
    bool synced_output;        /* mode 2026 */
    int  charset[2];           /* 0=ASCII 1=DEC 特殊图形 */
    int  charset_shift;        /* 0=G0 1=G1 */
    /* 制表位 */
    uint8_t *tabs;
    /* 回滚区环形缓冲 */
    VtLine  *sb;
    int      sb_cap, sb_len, sb_head;
    int      sb_limit;
    /* 解析器 */
    int      pstate;
    int      params[32];
    int      nparams;
    bool     param_seen[32];
    bool     param_set[32];
    bool     param_colon[32];
    int      cur_param_is_sub;  /* ':' 子参数，忽略 */
    char     interms[4];
    int      ninterms;
    bool     csi_private;
    char     osc[4096];
    int      osc_len;
    char     dcs[4096];
    int      dcs_len;
    int      dcs_passthrough;
    int      dcs_final;
    int      depth;
    uint8_t  u8buf[8];
    int      u8n;
    /* 视图/宿主信息 */
    int      pixel_w, pixel_h;
    char     title[256];
    uint32_t palette[256];
    bool     palette_custom[256];
    uint32_t osc_fg, osc_bg, osc_cursor;
    /* 脏区域 */
    int      dirty_y0, dirty_y1;
    bool     dirty;
    int      cursor_dirty;   /* 光标位置变化计数，视图用来通知输入法 */
    int      bell_count;
    void    *ud;
    VtWriteFn   write_cb;
    VtBellFn    bell_cb;
    VtTitleFn   title_cb;
    VtClipboardFn clipboard_cb;
    VtNotifyFn  notify_cb;
    VtDirtyFn   dirty_cb;
    VtPromptFn  prompt_cb;
} Vt;

/* ---------------- 生命周期 ---------------- */
Vt  *vt_new(int cols, int rows, int scrollback_limit);
void vt_free(Vt *vt);
void vt_resize(Vt *vt, int cols, int rows);
void vt_set_scrollback_limit(Vt *vt, int n);
void vt_reset(Vt *vt);

/* 喂 pty 输出。可以任意切分字节流。 */
void vt_input(Vt *vt, const void *data, size_t len);

/* ---------------- 视图访问 ---------------- */
int     vt_total_lines(Vt *vt);              /* 回滚区 + 屏幕 */
VtLine *vt_line_at(Vt *vt, int row);         /* row: 0 = 最老的回滚行 */
VtLine *vt_screen_line(Vt *vt, int y);       /* y: 0 = 屏幕第一行 */
int     vt_sb_offset(Vt *vt);                /* = sb_len */
int     vt_cursor_x(Vt *vt);
int     vt_cursor_y(Vt *vt);
bool    vt_cursor_visible(Vt *vt);
const char *vt_title(Vt *vt);
uint32_t vt_default_palette_entry(int idx);
bool    vt_take_dirty(Vt *vt, int *y0, int *y1);
void    vt_mark_all_dirty(Vt *vt);

/* 取一行/一段范围的文本(UTF-8, malloc)。 */
char *vt_row_text(Vt *vt, int row, bool trim_right);
char *vt_range_text(Vt *vt, int r0, int c0, int r1, int c1);

/* ---------------- 键 -> 字节 ---------------- */
#define VTM_SHIFT 1
#define VTM_ALT   2
#define VTM_CTRL  4
enum {
    VK_NONE = 0, VK_UP, VK_DOWN, VK_RIGHT, VK_LEFT, VK_HOME, VK_END,
    VK_INSERT, VK_DELETE, VK_PAGEUP, VK_PAGEDOWN, VK_BACKTAB,
    VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9,
    VK_F10, VK_F11, VK_F12, VK_ESC, VK_TAB, VK_ENTER, VK_BACKSPACE,
    VK_CHAR
};
int vt_key_encode(Vt *vt, int key, int mods, uint32_t ch, char *out, size_t outsz);

#ifdef __cplusplus
}
#endif
#endif /* VT_H */
