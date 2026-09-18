#include "vt.h"
#include "vt_unicode.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ---------------------------------------------------------------- */
/* 内联工具                                                          */
/* ---------------------------------------------------------------- */
static void vt_mark_dirty_rows(Vt *vt, int y0, int y1);
static void vt_scroll_up(Vt *vt, int n);
static void vt_scroll_down(Vt *vt, int n);
static void vt_erase_cells(Vt *vt, int y, int x0, int x1);
static void vt_linefeed(Vt *vt);
static void vt_put_char(Vt *vt, uint32_t cp);
static void vt_respond(Vt *vt, const char *fmt, ...);
static void vt_do_reset(Vt *vt, bool hard);

static const uint32_t kAnsi16[16] = {
    0x000000u, 0xCD0000u, 0x00CD00u, 0xCDCD00u, 0x0000EEu, 0xCD00CDu, 0x00CDCDu, 0xE5E5E5u,
    0x7F7F7Fu, 0xFF0000u, 0x00FF00u, 0xFFFF00u, 0x5C5CFFu, 0xFF00FFu, 0x00FFFFu, 0xFFFFFFu
};

static void vt_build_palette(Vt *vt)
{
    for (int i = 0; i < 16; i++) {
        vt->palette[i] = kAnsi16[i];
        vt->palette_custom[i] = false;
    }
    static const int steps[6] = { 0, 95, 135, 175, 215, 255 };
    int idx = 16;
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++, idx++)
                vt->palette[idx] = ((uint32_t)steps[r] << 16) | ((uint32_t)steps[g] << 8) | (uint32_t)steps[b];
    for (int i = 0; i < 24; i++, idx++) {
        uint32_t v = (uint32_t)(8 + i * 10);
        vt->palette[idx] = (v << 16) | (v << 8) | v;
    }
    for (int i = 16; i < 256; i++) vt->palette_custom[i] = false;
}

/* ---------------------------------------------------------------- */
/* 行 / 缓冲                                                         */
/* ---------------------------------------------------------------- */
static VtCell vt_blank_cell(uint32_t bg)
{
    VtCell c;
    c.cp = ' ';
    c.cp2 = 0;
    c.cp3 = 0;
    c.fg = VT_COLOR_DEFAULT;
    c.bg = bg;
    c.flags = 0;
    c.width = 1;
    c.reserved = 0;
    return c;
}

static VtLine vt_line_new(int cols, uint32_t bg)
{
    VtLine l;
    l.cols = (uint16_t)cols;
    l.flags = 0;
    l.rev = 1;
    l.cells = (VtCell *)malloc(sizeof(VtCell) * (size_t)cols);
    VtCell blank = vt_blank_cell(bg);
    for (int i = 0; i < cols; i++) l.cells[i] = blank;
    return l;
}

static void vt_line_free(VtLine *l)
{
    free(l->cells);
    l->cells = NULL;
    l->cols = 0;
}

static void vt_line_fill(VtLine *l, int from, int to, uint32_t bg)
{
    VtCell blank = vt_blank_cell(bg);
    if (from < 0) from = 0;
    if (to > l->cols) to = l->cols;
    for (int i = from; i < to; i++) l->cells[i] = blank;
}

static void vt_line_refit(VtLine *l, int newcols, uint32_t bg)
{
    if (l->cols == newcols) return;
    VtCell *nc = (VtCell *)malloc(sizeof(VtCell) * (size_t)newcols);
    VtCell blank = vt_blank_cell(bg);
    int copy = newcols < l->cols ? newcols : l->cols;
    for (int i = 0; i < copy; i++) nc[i] = l->cells[i];
    for (int i = copy; i < newcols; i++) nc[i] = blank;
    /* 如果宽字符被截断，把残缺的半个清掉 */
    if (newcols > 0 && copy == newcols && l->cols > newcols) {
        VtCell *last = &nc[newcols - 1];
        if (last->width == 2 && newcols >= 1) { last->width = 1; }
    }
    free(l->cells);
    l->cells = nc;
    l->cols = (uint16_t)newcols;
}

static void vt_buffer_init(Vt *vt, VtBuffer *b, int cols, int rows)
{
    b->cols = cols;
    b->rows = rows;
    b->lines = (VtLine *)calloc((size_t)rows, sizeof(VtLine));
    for (int i = 0; i < rows; i++) b->lines[i] = vt_line_new(cols, VT_COLOR_DEFAULT);
    b->cx = b->cy = 0;
    b->scroll_top = 0;
    b->scroll_bot = rows - 1;
    b->saved_cx = b->saved_cy = 0;
    b->saved_fg = b->saved_bg = VT_COLOR_DEFAULT;
    b->saved_flags = 0;
    b->saved_charset[0] = b->saved_charset[1] = 0;
    b->wrapped = 0;
}

static void vt_buffer_free(VtBuffer *b)
{
    if (!b->lines) return;
    for (int i = 0; i < b->rows; i++) vt_line_free(&b->lines[i]);
    free(b->lines);
    b->lines = NULL;
}

static void vt_buffer_reset(Vt *vt, VtBuffer *b, uint32_t bg)
{
    for (int i = 0; i < b->rows; i++) {
        vt_line_refit(&b->lines[i], b->cols, bg);
        vt_line_fill(&b->lines[i], 0, b->cols, bg);
        b->lines[i].flags = 0;
        b->lines[i].rev++;
    }
    b->cx = b->cy = 0;
    b->scroll_top = 0;
    b->scroll_bot = b->rows - 1;
    b->wrapped = 0;
}

/* ---------------- 回滚区(环形) ---------------- */

static void vt_sb_push(Vt *vt, VtLine line)
{
    if (vt->sb_limit <= 0) { vt_line_free(&line); return; }
    if (vt->sb_cap < vt->sb_limit) {
        int ncap = vt->sb_cap ? vt->sb_cap * 2 : 256;
        if (ncap > vt->sb_limit) ncap = vt->sb_limit;
        VtLine *nsb = (VtLine *)malloc(sizeof(VtLine) * (size_t)ncap);
        for (int i = 0; i < vt->sb_len; i++) nsb[i] = vt->sb[(vt->sb_head + i) % vt->sb_cap];
        free(vt->sb);
        vt->sb = nsb;
        vt->sb_cap = ncap;
        vt->sb_head = 0;
    }
    if (vt->sb_len == vt->sb_cap) {
        /* 丢掉最老的一行 */
        VtLine *old = &vt->sb[vt->sb_head];
        vt_line_free(old);
        *old = line;
        vt->sb_head = (vt->sb_head + 1) % vt->sb_cap;
    } else {
        vt->sb[(vt->sb_head + vt->sb_len) % vt->sb_cap] = line;
        vt->sb_len++;
    }
}

static VtLine *vt_sb_line(Vt *vt, int i)
{
    return &vt->sb[(vt->sb_head + i) % vt->sb_cap];
}

static void vt_sb_clear(Vt *vt)
{
    for (int i = 0; i < vt->sb_len; i++) vt_line_free(vt_sb_line(vt, i));
    vt->sb_len = 0;
    vt->sb_head = 0;
}

/* ---------------------------------------------------------------- */
/* 脏区域                                                           */
/* ---------------------------------------------------------------- */
static void vt_mark_dirty_rows(Vt *vt, int y0, int y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 >= vt->buf->rows) y1 = vt->buf->rows - 1;
    if (y1 < y0) return;
    if (!vt->dirty) {
        vt->dirty = true;
        vt->dirty_y0 = y0;
        vt->dirty_y1 = y1;
    } else {
        if (y0 < vt->dirty_y0) vt->dirty_y0 = y0;
        if (y1 > vt->dirty_y1) vt->dirty_y1 = y1;
    }
    if (vt->dirty_cb && !vt->synced_output) vt->dirty_cb(vt->ud);
}

void vt_mark_all_dirty(Vt *vt)
{
    vt_mark_dirty_rows(vt, 0, vt->buf->rows - 1);
}

bool vt_take_dirty(Vt *vt, int *y0, int *y1)
{
    if (!vt->dirty) return false;
    if (y0) *y0 = vt->dirty_y0;
    if (y1) *y1 = vt->dirty_y1;
    vt->dirty = false;
    vt->dirty_y0 = 1 << 30;
    vt->dirty_y1 = -1;
    return true;
}

/* ---------------------------------------------------------------- */
/* 生命周期                                                          */
/* ---------------------------------------------------------------- */
Vt *vt_new(int cols, int rows, int scrollback_limit)
{
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    Vt *vt = (Vt *)calloc(1, sizeof(Vt));
    vt->cols = cols;
    vt->rows = rows;
    vt->sb_limit = scrollback_limit < 0 ? 0 : scrollback_limit;
    vt_build_palette(vt);
    vt_buffer_init(vt, &vt->normal, cols, rows);
    vt_buffer_init(vt, &vt->alt, cols, rows);
    vt->buf = &vt->normal;
    vt->tabs = (uint8_t *)malloc((size_t)cols);
    vt->dirty_y0 = 1 << 30;
    vt->dirty_y1 = -1;
    vt_do_reset(vt, true);
    return vt;
}

void vt_free(Vt *vt)
{
    if (!vt) return;
    vt_sb_clear(vt);
    free(vt->sb);
    vt_buffer_free(&vt->normal);
    vt_buffer_free(&vt->alt);
    free(vt->tabs);
    free(vt);
}

void vt_set_scrollback_limit(Vt *vt, int n)
{
    if (n < 0) n = 0;
    vt->sb_limit = n;
    while (vt->sb_len > vt->sb_limit) {
        VtLine *old = vt_sb_line(vt, 0);
        vt_line_free(old);
        vt->sb_head = (vt->sb_head + 1) % vt->sb_cap;
        vt->sb_len--;
    }
}

void vt_reset(Vt *vt)
{
    vt_do_reset(vt, true);
}

int vt_total_lines(Vt *vt) { return vt->alt_active ? vt->buf->rows : vt->sb_len + vt->buf->rows; }
int vt_sb_offset(Vt *vt) { return vt->alt_active ? 0 : vt->sb_len; }

VtLine *vt_line_at(Vt *vt, int row)
{
    if (row < 0) return NULL;
    if (vt->alt_active) {
        if (row >= vt->buf->rows) return NULL;
        return &vt->buf->lines[row];
    }
    if (row < vt->sb_len) return vt_sb_line(vt, row);
    row -= vt->sb_len;
    if (row >= vt->buf->rows) return NULL;
    return &vt->buf->lines[row];
}

VtLine *vt_screen_line(Vt *vt, int y)
{
    if (y < 0 || y >= vt->buf->rows) return NULL;
    return &vt->buf->lines[y];
}

int vt_cursor_x(Vt *vt) { return vt->buf->cx; }
int vt_cursor_y(Vt *vt) { return vt->buf->cy; }
bool vt_cursor_visible(Vt *vt) { return vt->cursor_visible; }
const char *vt_title(Vt *vt) { return vt->title; }
uint32_t vt_default_palette_entry(int idx)
{
    if (idx < 0 || idx > 255) return 0xFFFFFFu;
    if (idx < 16) return kAnsi16[idx];
    if (idx < 232) {
        static const int steps[6] = { 0, 95, 135, 175, 215, 255 };
        int i = idx - 16;
        return ((uint32_t)steps[i / 36] << 16) | ((uint32_t)steps[(i / 6) % 6] << 8) |
               (uint32_t)steps[i % 6];
    }
    uint32_t v = (uint32_t)(8 + (idx - 232) * 10);
    return (v << 16) | (v << 8) | v;
}

/* ---------------------------------------------------------------- */
/* 光标 / 滚动                                                       */
/* ---------------------------------------------------------------- */
static void vt_respond(Vt *vt, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    if (vt->write_cb) vt->write_cb(vt->ud, buf, (size_t)n);
}

static void vt_cursor_set(Vt *vt, int x, int y)
{
    VtBuffer *b = vt->buf;
    if (x < 0) x = 0;
    if (x > b->cols - 1) x = b->cols - 1;
    if (y < 0) y = 0;
    if (y > b->rows - 1) y = b->rows - 1;
    b->wrapped = 0;
    if (b->cx != x || b->cy != y) {
        b->cx = x;
        b->cy = y;
        vt->cursor_dirty++;
        vt_mark_dirty_rows(vt, y, y);
    }
}

static void vt_scroll_up(Vt *vt, int n)
{
    VtBuffer *b = vt->buf;
    int top = b->scroll_top, bot = b->scroll_bot;
    if (n <= 0) return;
    if (n > bot - top + 1) n = bot - top + 1;
    for (int k = 0; k < n; k++) {
        VtLine old = b->lines[top];
        if (bot > top)
            memmove(&b->lines[top], &b->lines[top + 1], sizeof(VtLine) * (size_t)(bot - top));
        b->lines[bot] = vt_line_new(b->cols, VT_COLOR_DEFAULT);
        b->lines[bot].rev = old.rev + 1;
        if (top == 0 && bot == b->rows - 1 && b == &vt->normal)
            vt_sb_push(vt, old);
        else
            vt_line_free(&old);
    }
    vt_mark_dirty_rows(vt, 0, b->rows - 1);
}

static void vt_scroll_down(Vt *vt, int n)
{
    VtBuffer *b = vt->buf;
    int top = b->scroll_top, bot = b->scroll_bot;
    if (n <= 0) return;
    if (n > bot - top + 1) n = bot - top + 1;
    for (int k = 0; k < n; k++) {
        VtLine old = b->lines[bot];
        if (bot > top)
            memmove(&b->lines[top + 1], &b->lines[top], sizeof(VtLine) * (size_t)(bot - top));
        b->lines[top] = vt_line_new(b->cols, VT_COLOR_DEFAULT);
        b->lines[top].rev = old.rev + 1;
        vt_line_free(&old);
    }
    vt_mark_dirty_rows(vt, 0, b->rows - 1);
}

static void vt_linefeed(Vt *vt)
{
    VtBuffer *b = vt->buf;
    b->wrapped = 0;
    if (b->cy == b->scroll_bot) vt_scroll_up(vt, 1);
    else if (b->cy < b->rows - 1) {
        b->cy++;
        vt->cursor_dirty++;
        vt_mark_dirty_rows(vt, b->cy, b->cy);
    }
}

static void vt_reverse_index(Vt *vt)
{
    VtBuffer *b = vt->buf;
    b->wrapped = 0;
    if (b->cy == b->scroll_top) vt_scroll_down(vt, 1);
    else if (b->cy > 0) {
        b->cy--;
        vt->cursor_dirty++;
        vt_mark_dirty_rows(vt, b->cy, b->cy);
    }
}

static void vt_erase_cells(Vt *vt, int y, int x0, int x1)
{
    VtBuffer *b = vt->buf;
    if (y < 0 || y >= b->rows) return;
    if (x0 < 0) x0 = 0;
    if (x1 > b->cols) x1 = b->cols;
    if (x1 <= x0) return;
    VtLine *l = &b->lines[y];
    VtCell blank = vt_blank_cell(vt->cur_bg);
    for (int x = x0; x < x1; x++) l->cells[x] = blank;
    l->rev++;
    vt_mark_dirty_rows(vt, y, y);
}

/* ---------------------------------------------------------------- */
/* 打印字符                                                          */
/* ---------------------------------------------------------------- */
static VtCell *vt_prev_cell(Vt *vt, int *out_y)
{
    VtBuffer *b = vt->buf;
    int px = b->cx - 1, py = b->cy;
    if (px < 0) {
        if (py > 0 && (b->lines[py - 1].flags & VT_LINE_WRAPPED)) {
            py--;
            px = b->cols - 1;
        } else {
            return NULL;
        }
    }
    /* 落在宽字符的后半格上时，回到宽字符本身 */
    if (px > 0 && b->lines[py].cells[px].width == 0 &&
        (b->lines[py].cells[px].flags & VT_WIDE_CONT))
        px--;
    if (out_y) *out_y = py;
    return &b->lines[py].cells[px];
}

/* 写入前把被覆盖的宽字符的另外半格擦掉，避免出现半个字 */
static void vt_fix_wide_neighbors(Vt *vt, int y, int x)
{
    VtLine *l = &vt->buf->lines[y];
    if (x < 0 || x >= vt->buf->cols) return;
    VtCell *c = &l->cells[x];
    if (c->width == 0 && (c->flags & VT_WIDE_CONT) && x > 0) {
        VtCell blank = vt_blank_cell(l->cells[x - 1].bg);
        l->cells[x - 1] = blank;
    }
    if (c->width == 2 && x + 1 < vt->buf->cols) {
        VtCell blank = vt_blank_cell(l->cells[x + 1].bg);
        l->cells[x + 1] = blank;
    }
}

static uint32_t vt_cell_last_cp(const VtCell *c)
{
    if (c->cp3) return c->cp3;
    if (c->cp2) return c->cp2;
    return c->cp;
}

static void vt_cell_add_cp(VtCell *c, uint32_t cp)
{
    if (c->cp2 == 0) c->cp2 = cp;
    else if (c->cp3 == 0) c->cp3 = cp;
}

static void vt_cell_insert_at(Vt *vt, int y, int x, int n)
{
    VtBuffer *b = vt->buf;
    VtLine *l = &b->lines[y];
    if (x >= b->cols || n <= 0) return;
    if (n > b->cols - x) n = b->cols - x;
    memmove(&l->cells[x + n], &l->cells[x], sizeof(VtCell) * (size_t)(b->cols - x - n));
    VtCell blank = vt_blank_cell(vt->cur_bg);
    for (int i = 0; i < n; i++) l->cells[x + i] = blank;
    l->rev++;
    vt_mark_dirty_rows(vt, y, y);
}

static void vt_cell_delete_at(Vt *vt, int y, int x, int n)
{
    VtBuffer *b = vt->buf;
    VtLine *l = &b->lines[y];
    if (x >= b->cols || n <= 0) return;
    if (n > b->cols - x) n = b->cols - x;
    memmove(&l->cells[x], &l->cells[x + n], sizeof(VtCell) * (size_t)(b->cols - x - n));
    VtCell blank = vt_blank_cell(vt->cur_bg);
    for (int i = b->cols - n; i < b->cols; i++) l->cells[i] = blank;
    l->rev++;
    vt_mark_dirty_rows(vt, y, y);
}

static void vt_put_char(Vt *vt, uint32_t cp)
{
    VtBuffer *b = vt->buf;
    int w = vt_cell_width(cp);

    if (w == 0) {
        int py = 0;
        VtCell *prev = vt_prev_cell(vt, &py);
        if (!prev) return;
        if (prev->cp == 0 || prev->cp == ' ') return;
        vt_cell_add_cp(prev, cp);
        b->lines[py].rev++;
        vt_mark_dirty_rows(vt, py, py);
        return;
    }
    if (w < 0) return;

    /* ZWJ 序列：并进前一格，不再占位 */
    {
        int py = 0;
        VtCell *prev = vt_prev_cell(vt, &py);
        if (prev && prev->cp != 0 && prev->cp != ' ' && vt_cell_last_cp(prev) == 0x200D) {
            if (prev->cp2 == 0 || prev->cp3 == 0) {
                vt_cell_add_cp(prev, cp);
                b->lines[py].rev++;
                vt_mark_dirty_rows(vt, py, py);
                return;
            }
        }
    }

    if (b->wrapped) {
        b->lines[b->cy].flags |= VT_LINE_WRAPPED;
        b->lines[b->cy].rev++;
        vt_linefeed(vt);
        b->cx = 0;
    }

    if (w == 2 && b->cx == b->cols - 1) {
        if (vt->autowrap) {
            /* 宽字符放不下，本行末尾空出来的格子只是填充，重新折行时要丢掉 */
            VtCell *pad = &b->lines[b->cy].cells[b->cx];
            if (pad->cp == ' ' && pad->cp2 == 0 && pad->cp3 == 0)
                pad->flags |= VT_WIDE_PAD;
            b->lines[b->cy].flags |= VT_LINE_WRAPPED;
            b->lines[b->cy].rev++;
            vt_linefeed(vt);
            b->cx = 0;
        } else {
            b->cx = b->cols >= 2 ? b->cols - 2 : 0;
        }
    }

    if (vt->insert) vt_cell_insert_at(vt, b->cy, b->cx, w);

    VtLine *l = &b->lines[b->cy];
    vt_fix_wide_neighbors(vt, b->cy, b->cx);
    if (w == 2) vt_fix_wide_neighbors(vt, b->cy, b->cx + 1);
    VtCell *c = &l->cells[b->cx];
    c->cp = cp;
    c->cp2 = 0;
    c->cp3 = 0;
    c->fg = vt->cur_fg;
    c->bg = vt->cur_bg;
    c->flags = vt->cur_flags;
    c->width = (uint8_t)w;
    if (w == 2 && b->cx + 1 < b->cols) {
        VtCell *c2 = &l->cells[b->cx + 1];
        *c2 = *c;
        c2->cp = 0;
        c2->cp2 = 0;
        c2->cp3 = 0;
        c2->width = 0;
        c2->flags = (uint16_t)(c->flags | VT_WIDE_CONT);
    }
    l->rev++;
    vt_mark_dirty_rows(vt, b->cy, b->cy);

    if (w == 2 && b->cx + 1 >= b->cols) {
        b->wrapped = vt->autowrap ? 1 : 0;
    } else if (b->cx + w >= b->cols) {
        if (vt->autowrap) {
            b->wrapped = 1;
        } else {
            b->cx = b->cols - 1;
        }
    } else {
        b->cx += w;
    }
}

/* ---------------------------------------------------------------- */
/* SGR                                                              */
/* ---------------------------------------------------------------- */
static void vt_reset_attrs(Vt *vt)
{
    vt->cur_fg = VT_COLOR_DEFAULT;
    vt->cur_bg = VT_COLOR_DEFAULT;
    vt->cur_flags = 0;
}

/* 读 38/48 后面的颜色，支持 "38;5;n"、"38:5:n"、"38;2;r;g;b"、"38:2::r:g:b" */
static bool vt_read_color(Vt *vt, int *idx, uint32_t *out)
{
    int i = *idx;
    if (i >= vt->nparams) return false;
    int mode = vt->params[i];
    if (mode == 5 && i + 1 < vt->nparams) {
        *out = VT_COLOR_IDX(vt->params[i + 1]);
        i += 2;
    } else if (mode == 2) {
        int j = i + 1;
        if (j < vt->nparams && !vt->param_set[j]) j++;    /* 冒号语法里的空槽=颜色空间 */
        if (j + 2 >= vt->nparams) return false;
        *out = VT_COLOR_RGB(vt->params[j], vt->params[j + 1], vt->params[j + 2]);
        i = j + 3;
    } else {
        return false;
    }
    while (i < vt->nparams && vt->param_colon[i]) i++;
    *idx = i;
    return true;
}

static void vt_sgr(Vt *vt)
{
    if (vt->nparams == 0) { vt_reset_attrs(vt); return; }
    for (int i = 0; i < vt->nparams; ) {
        int p = vt->params[i];
        if (!vt->param_set[i] && p == 0) { i++; continue; }
        if (p == 0) { vt_reset_attrs(vt); i++; continue; }
        if (p == 1) { vt->cur_flags |= VT_BOLD; i++; continue; }
        if (p == 2) { vt->cur_flags |= VT_DIM; i++; continue; }
        if (p == 3) { vt->cur_flags |= VT_ITALIC; i++; continue; }
        if (p == 4) {
            if (vt->param_colon[i + 1] && i + 1 < vt->nparams) {
                int style = vt->params[i + 1];
                if (style == 0) vt->cur_flags &= (uint16_t)~VT_UNDERLINE;
                else vt->cur_flags |= VT_UNDERLINE;
                i += 2;
                while (i < vt->nparams && vt->param_colon[i]) i++;
            } else {
                vt->cur_flags |= VT_UNDERLINE;
                i++;
            }
            continue;
        }
        if (p == 5 || p == 6) { vt->cur_flags |= VT_BLINK; i++; continue; }
        if (p == 7) { vt->cur_flags |= VT_REVERSE; i++; continue; }
        if (p == 8) { vt->cur_flags |= VT_INVISIBLE; i++; continue; }
        if (p == 9) { vt->cur_flags |= VT_STRIKE; i++; continue; }
        if (p == 21) { vt->cur_flags |= VT_DBLUNDER; i++; continue; }
        if (p == 22) { vt->cur_flags &= (uint16_t)~(VT_BOLD | VT_DIM); i++; continue; }
        if (p == 23) { vt->cur_flags &= (uint16_t)~VT_ITALIC; i++; continue; }
        if (p == 24) { vt->cur_flags &= (uint16_t)~(VT_UNDERLINE | VT_DBLUNDER); i++; continue; }
        if (p == 25) { vt->cur_flags &= (uint16_t)~VT_BLINK; i++; continue; }
        if (p == 27) { vt->cur_flags &= (uint16_t)~VT_REVERSE; i++; continue; }
        if (p == 28) { vt->cur_flags &= (uint16_t)~VT_INVISIBLE; i++; continue; }
        if (p == 29) { vt->cur_flags &= (uint16_t)~VT_STRIKE; i++; continue; }
        if (p >= 30 && p <= 37) { vt->cur_fg = VT_COLOR_IDX(p - 30); i++; continue; }
        if (p == 38) { i++; uint32_t c; if (vt_read_color(vt, &i, &c)) vt->cur_fg = c; continue; }
        if (p == 39) { vt->cur_fg = VT_COLOR_DEFAULT; i++; continue; }
        if (p >= 40 && p <= 47) { vt->cur_bg = VT_COLOR_IDX(p - 40); i++; continue; }
        if (p == 48) { i++; uint32_t c; if (vt_read_color(vt, &i, &c)) vt->cur_bg = c; continue; }
        if (p == 49) { vt->cur_bg = VT_COLOR_DEFAULT; i++; continue; }
        if (p >= 90 && p <= 97) { vt->cur_fg = VT_COLOR_IDX(p - 90 + 8); i++; continue; }
        if (p >= 100 && p <= 107) { vt->cur_bg = VT_COLOR_IDX(p - 100 + 8); i++; continue; }
        if (p == 58) { i++; uint32_t c; vt_read_color(vt, &i, &c); continue; }  /* 下划线颜色，忽略 */
        i++;
    }
}

/* ---------------------------------------------------------------- */
/* 模式与备用屏                                                      */
/* ---------------------------------------------------------------- */
static void vt_set_alt_screen(Vt *vt, bool on, bool save_cursor, bool clear)
{
    if (on == vt->alt_active) return;
    VtBuffer *b = vt->buf;
    if (save_cursor) {
        b->saved_cx = b->cx;
        b->saved_cy = b->cy;
        b->saved_fg = vt->cur_fg;
        b->saved_bg = vt->cur_bg;
        b->saved_flags = vt->cur_flags;
    }
    vt->alt_active = on;
    vt->buf = on ? &vt->alt : &vt->normal;
    VtBuffer *nb = vt->buf;
    if (clear) vt_buffer_reset(vt, nb, vt->cur_bg);
    nb->scroll_top = 0;
    nb->scroll_bot = nb->rows - 1;
    nb->wrapped = 0;
    if (!on && save_cursor) {
        nb->cx = nb->saved_cx;
        nb->cy = nb->saved_cy;
        vt->cur_fg = nb->saved_fg;
        vt->cur_bg = nb->saved_bg;
        vt->cur_flags = nb->saved_flags;
    }
    vt_mark_all_dirty(vt);
    vt->cursor_dirty++;
}

static void vt_set_mode(Vt *vt, int mode, bool set, bool priv)
{
    if (!priv) {
        switch (mode) {
        case 4: vt->insert = set; break;
        case 20: vt->newline_mode = set; break;
        default: break;
        }
        return;
    }
    switch (mode) {
    case 1: vt->appcursor = set; break;
    case 5: vt->reverse_video = set; vt_mark_all_dirty(vt); break;
    case 6:
        vt->origin = set;
        vt_cursor_set(vt, 0, set ? vt->buf->scroll_top : 0);
        break;
    case 7: vt->autowrap = set; break;
    case 9: vt->mouse_mode = set ? 9 : 0; break;
    case 12: break;                      /* 光标闪烁 */
    case 25: vt->cursor_visible = set; vt->cursor_dirty++; vt_mark_all_dirty(vt); break;
    case 40: break;
    case 47:
        if (!vt->alt_active && set) vt_set_alt_screen(vt, true, false, false);
        else if (vt->alt_active && !set) vt_set_alt_screen(vt, false, false, false);
        break;
    case 1000: case 1002: case 1003:
        if (set) { vt->mouse_mode = mode; if (mode != 1000) vt->mouse_mode = mode; }
        else if (vt->mouse_mode == mode) vt->mouse_mode = 0;
        break;
    case 1004: vt->focus_events = set; break;
    case 1005: vt->mouse_utf8 = set; break;
    case 1006: vt->mouse_sgr = set; break;
    case 1015: vt->mouse_urxvt = set; break;
    case 1016: break;
    case 1036: case 1039: break;         /* meta 键发送 esc */
    case 1047:
        if (set && !vt->alt_active) vt_set_alt_screen(vt, true, false, true);
        else if (!set && vt->alt_active) vt_set_alt_screen(vt, false, false, false);
        break;
    case 1048:
        if (set) {
            vt->buf->saved_cx = vt->buf->cx;
            vt->buf->saved_cy = vt->buf->cy;
            vt->buf->saved_fg = vt->cur_fg;
            vt->buf->saved_bg = vt->cur_bg;
            vt->buf->saved_flags = vt->cur_flags;
        } else {
            vt->cur_fg = vt->buf->saved_fg;
            vt->cur_bg = vt->buf->saved_bg;
            vt->cur_flags = vt->buf->saved_flags;
            vt_cursor_set(vt, vt->buf->saved_cx, vt->buf->saved_cy);
        }
        break;
    case 1049:
        if (set && !vt->alt_active) vt_set_alt_screen(vt, true, true, true);
        else if (!set && vt->alt_active) vt_set_alt_screen(vt, false, true, false);
        break;
    case 2004: vt->bracketed_paste = set; break;
    case 2026:
        vt->synced_output = set;
        if (!set) {
            vt->dirty = true;
            vt->dirty_y0 = 0;
            vt->dirty_y1 = vt->buf->rows - 1;
            if (vt->dirty_cb) vt->dirty_cb(vt->ud);
        }
        break;
    default: break;
    }
}

static void vt_do_reset(Vt *vt, bool hard)
{
    vt_reset_attrs(vt);
    vt->autowrap = true;
    vt->origin = false;
    vt->insert = false;
    vt->appcursor = false;
    vt->appkeypad = false;
    vt->cursor_visible = true;
    vt->reverse_video = false;
    vt->newline_mode = false;
    vt->mouse_mode = 0;
    vt->mouse_sgr = false;
    vt->mouse_urxvt = false;
    vt->mouse_utf8 = false;
    vt->focus_events = false;
    vt->bracketed_paste = false;
    vt->synced_output = false;
    vt->cursor_style = 0;
    vt->charset[0] = 0;
    vt->charset[1] = 0;
    vt->charset_shift = 0;
    for (int i = 0; i < vt->cols; i++) vt->tabs[i] = ((i % 8) == 0);
    if (hard) {
        if (vt->alt_active) vt_set_alt_screen(vt, false, false, false);
        vt_sb_clear(vt);
        vt_buffer_reset(vt, &vt->normal, VT_COLOR_DEFAULT);
        vt_buffer_reset(vt, &vt->alt, VT_COLOR_DEFAULT);
        vt->buf = &vt->normal;
        vt->alt_active = false;
        vt_build_palette(vt);
    } else {
        VtBuffer *b = vt->buf;
        b->scroll_top = 0;
        b->scroll_bot = b->rows - 1;
        b->cx = b->cy = 0;
        b->wrapped = 0;
        for (int y = 0; y < b->rows; y++) {
            vt_line_fill(&b->lines[y], 0, b->cols, VT_COLOR_DEFAULT);
            b->lines[y].flags = 0;
            b->lines[y].rev++;
        }
    }
    vt_mark_all_dirty(vt);
    vt->cursor_dirty++;
}

/* DEC 特殊图形字符集，`ls`/`dialog` 画框框要用 */
static const uint32_t kDECSpecial[32] = {
    0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1,
    0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA,
    0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C,
    0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7, 0x0020
};

static uint32_t vt_map_charset(Vt *vt, uint32_t cp)
{
    int cs = vt->charset[vt->charset_shift];
    if (cs == 1 && cp >= 0x60 && cp <= 0x7E) return kDECSpecial[cp - 0x60];
    return cp;
}

/* ---------------------------------------------------------------- */
/* 序列分发                                                          */
/* ---------------------------------------------------------------- */
#define CPARAM(i, d) (((i) < vt->nparams && vt->param_set[i]) ? vt->params[i] : (d))

static void vt_tab_forward(Vt *vt, int n)
{
    VtBuffer *b = vt->buf;
    while (n-- > 0) {
        int x = b->cx;
        do { x++; } while (x < b->cols - 1 && !vt->tabs[x]);
        if (x >= b->cols) x = b->cols - 1;
        b->cx = x;
    }
    b->wrapped = 0;
    vt->cursor_dirty++;
}

static void vt_tab_backward(Vt *vt, int n)
{
    VtBuffer *b = vt->buf;
    while (n-- > 0) {
        int x = b->cx;
        do { x--; } while (x > 0 && !vt->tabs[x]);
        if (x < 0) x = 0;
        b->cx = x;
    }
    b->wrapped = 0;
    vt->cursor_dirty++;
}

static void vt_scroll_region_default(Vt *vt)
{
    vt->buf->scroll_top = 0;
    vt->buf->scroll_bot = vt->buf->rows - 1;
}

static void vt_csi_dispatch(Vt *vt, uint8_t final)
{
    VtBuffer *b = vt->buf;
    bool priv = vt->csi_private;
    switch (final) {
    case '@':
        vt_cell_insert_at(vt, b->cy, b->cx, CPARAM(0, 1));
        break;
    case 'A': {
        int top = vt->origin ? b->scroll_top : 0;
        int n = CPARAM(0, 1);
        int y = b->cy - n;
        vt_cursor_set(vt, b->cx, y < top ? top : y);
        break;
    }
    case 'B': {
        int bot = vt->origin ? b->scroll_bot : b->rows - 1;
        int n = CPARAM(0, 1);
        int y = b->cy + n;
        vt_cursor_set(vt, b->cx, y > bot ? bot : y);
        break;
    }
    case 'C': vt_cursor_set(vt, b->cx + CPARAM(0, 1), b->cy); break;
    case 'D': vt_cursor_set(vt, b->cx - CPARAM(0, 1), b->cy); break;
    case 'E': vt_cursor_set(vt, 0, b->cy + CPARAM(0, 1)); break;
    case 'F': vt_cursor_set(vt, 0, b->cy - CPARAM(0, 1)); break;
    case 'G': case '`': vt_cursor_set(vt, CPARAM(0, 1) - 1, b->cy); break;
    case 'd': vt_cursor_set(vt, b->cx, CPARAM(0, 1) - 1 + (vt->origin ? b->scroll_top : 0)); break;
    case 'H': case 'f': {
        int y = CPARAM(0, 1) - 1;
        int x = CPARAM(1, 1) - 1;
        if (vt->origin) y += b->scroll_top;
        vt_cursor_set(vt, x, y);
        break;
    }
    case 'I': vt_tab_forward(vt, CPARAM(0, 1)); break;
    case 'Z': vt_tab_backward(vt, CPARAM(0, 1)); break;
    case 'J': {
        int mode = CPARAM(0, 0);
        if (mode == 0) {
            vt_erase_cells(vt, b->cy, b->cx, b->cols);
            for (int y = b->cy + 1; y < b->rows; y++) vt_erase_cells(vt, y, 0, b->cols);
        } else if (mode == 1) {
            for (int y = 0; y < b->cy; y++) vt_erase_cells(vt, y, 0, b->cols);
            vt_erase_cells(vt, b->cy, 0, b->cx + 1);
        } else if (mode == 2) {
            for (int y = 0; y < b->rows; y++) vt_erase_cells(vt, y, 0, b->cols);
        } else if (mode == 3) {
            vt_sb_clear(vt);
            b->wrapped = 0;
        }
        break;
    }
    case 'K': {
        int mode = CPARAM(0, 0);
        if (mode == 0) vt_erase_cells(vt, b->cy, b->cx, b->cols);
        else if (mode == 1) vt_erase_cells(vt, b->cy, 0, b->cx + 1);
        else if (mode == 2) vt_erase_cells(vt, b->cy, 0, b->cols);
        break;
    }
    case 'L': {
        int n = CPARAM(0, 1);
        if (b->cy >= b->scroll_top && b->cy <= b->scroll_bot) {
            int save_top = b->scroll_top, save_bot = b->scroll_bot;
            b->scroll_top = b->cy;
            vt_scroll_down(vt, n);
            b->scroll_top = save_top;
            b->scroll_bot = save_bot;
            vt_erase_cells(vt, b->cy, 0, b->cols);
        }
        break;
    }
    case 'M': {
        int n = CPARAM(0, 1);
        if (b->cy >= b->scroll_top && b->cy <= b->scroll_bot) {
            int save_top = b->scroll_top, save_bot = b->scroll_bot;
            b->scroll_top = b->cy;
            vt_scroll_up(vt, n);
            b->scroll_top = save_top;
            b->scroll_bot = save_bot;
        }
        break;
    }
    case 'P': vt_cell_delete_at(vt, b->cy, b->cx, CPARAM(0, 1)); break;
    case 'S': vt_scroll_up(vt, CPARAM(0, 1)); break;
    case 'T': vt_scroll_down(vt, CPARAM(0, 1)); break;
    case 'X': vt_erase_cells(vt, b->cy, b->cx, b->cx + CPARAM(0, 1)); break;
    case 'b': {
        int n = CPARAM(0, 1);
        if (b->cx > 0 && n > 0) {
            VtCell prev = b->lines[b->cy].cells[b->cx - 1];
            prev.flags = (uint16_t)(prev.flags & ~VT_WIDE_CONT);
            for (int i = 0; i < n; i++) vt_put_char(vt, prev.cp);
        }
        break;
    }
    case 'c':
        if (priv || vt->ninterms > 0) vt_respond(vt, "\x1b[>0;136;0c");
        else vt_respond(vt, "\x1b[?62;1;2;6;9;15;18;21;22c");
        break;
    case 'g':
        if (CPARAM(0, 0) == 0) vt->tabs[b->cx] = 0;
        else if (CPARAM(0, 0) == 3) for (int i = 0; i < vt->cols; i++) vt->tabs[i] = 0;
        break;
    case 'h': case 'l': {
        bool set = (final == 'h');
        for (int i = 0; i < vt->nparams; i++) vt_set_mode(vt, vt->params[i], set, priv);
        if (vt->nparams == 0 && set && vt->ninterms > 0) { /* \e[?h 之类，忽略 */ }
        break;
    }
    case 'm': vt_sgr(vt); break;
    case 'n': {
        int mode = CPARAM(0, 0);
        if (mode == 5) vt_respond(vt, "\x1b[0n");
        else if (mode == 6) {
            int y = b->cy + 1 + (vt->origin ? 0 : 0);
            int x = b->cx + 1;
            if (priv) vt_respond(vt, "\x1b[?%d;%dR", y, x);
            else vt_respond(vt, "\x1b[%d;%dR", y, x);
        }
        break;
    }
    case 'p': {
        if (vt->ninterms > 0 && vt->interms[0] == '!') { vt_do_reset(vt, false); break; }
        if (vt->ninterms > 0 && vt->interms[0] == '$') {   /* DECRQM */
            int mode = CPARAM(0, 0);
            int state = 0;
            switch (mode) {
            case 1: state = vt->appcursor ? 1 : 2; break;
            case 6: state = vt->origin ? 1 : 2; break;
            case 7: state = vt->autowrap ? 1 : 2; break;
            case 25: state = vt->cursor_visible ? 1 : 2; break;
            case 1000: case 1002: case 1003: state = (vt->mouse_mode == mode) ? 1 : 2; break;
            case 1006: state = vt->mouse_sgr ? 1 : 2; break;
            case 2004: state = vt->bracketed_paste ? 1 : 2; break;
            case 2026: state = vt->synced_output ? 1 : 2; break;
            case 1049: state = vt->alt_active ? 1 : 2; break;
            default: state = 0; break;
            }
            if (priv) vt_respond(vt, "\x1b[?%d;%d$y", mode, state);
            else vt_respond(vt, "\x1b[%d;%d$y", mode, state);
            break;
        }
        break;
    }
    case 'q':
        if (vt->ninterms > 0 && vt->interms[0] == ' ') vt->cursor_style = CPARAM(0, 0);
        break;
    case 'r': {
        int top = CPARAM(0, 1) - 1;
        int bot = CPARAM(1, b->rows) - 1;
        if (top < 0) top = 0;
        if (bot > b->rows - 1) bot = b->rows - 1;
        if (top < bot) {
            b->scroll_top = top;
            b->scroll_bot = bot;
        } else {
            vt_scroll_region_default(vt);
        }
        vt_cursor_set(vt, 0, vt->origin ? b->scroll_top : 0);
        break;
    }
    case 's':
        b->saved_cx = b->cx;
        b->saved_cy = b->cy;
        b->saved_fg = vt->cur_fg;
        b->saved_bg = vt->cur_bg;
        b->saved_flags = vt->cur_flags;
        break;
    case 'u':
        vt->cur_fg = b->saved_fg;
        vt->cur_bg = b->saved_bg;
        vt->cur_flags = b->saved_flags;
        vt_cursor_set(vt, b->saved_cx, b->saved_cy);
        break;
    case 't': {
        int op = CPARAM(0, 0);
        if (op == 18) vt_respond(vt, "\x1b[8;%d;%dt", b->rows, b->cols);
        else if (op == 14) vt_respond(vt, "\x1b[4;%d;%dt", vt->pixel_h, vt->pixel_w);
        else if (op == 21) vt_respond(vt, "l");
        break;
    }
    default: break;
    }
}

static void vt_esc_dispatch(Vt *vt, uint8_t final)
{
    char interm = vt->ninterms > 0 ? vt->interms[0] : 0;
    if (interm == '(' || interm == ')' || interm == '*' || interm == '+') {
        int which = (interm == '(' || interm == '*') ? 0 : 1;
        int cs = 0;
        if (final == '0') cs = 1;
        else if (final == 'B' || final == 'A' || final == 'U' || final == 'K' || final == 'Q') cs = 0;
        else cs = 0;
        vt->charset[which] = cs;
        return;
    }
    if (interm == '#') {
        if (final == '8') {
            for (int y = 0; y < vt->buf->rows; y++) {
                for (int x = 0; x < vt->buf->cols; x++) {
                    VtCell *c = &vt->buf->lines[y].cells[x];
                    c->cp = 'E';
                    c->cp2 = c->cp3 = 0;
                    c->fg = VT_COLOR_DEFAULT;
                    c->bg = VT_COLOR_DEFAULT;
                    c->flags = 0;
                    c->width = 1;
                }
                vt->buf->lines[y].rev++;
            }
            vt_mark_all_dirty(vt);
        }
        return;
    }
    switch (final) {
    case 'D': vt_linefeed(vt); break;
    case 'E': vt_cursor_set(vt, 0, vt->buf->cy); vt_linefeed(vt); break;
    case 'H': if (vt->buf->cx < vt->cols) vt->tabs[vt->buf->cx] = 1; break;
    case 'M': vt_reverse_index(vt); break;
    case 'N': case 'O': break;
    case 'Z': vt_respond(vt, "\x1b[?62;1;2;6;9;15;18;21;22c"); break;
    case '7':
        vt->buf->saved_cx = vt->buf->cx;
        vt->buf->saved_cy = vt->buf->cy;
        vt->buf->saved_fg = vt->cur_fg;
        vt->buf->saved_bg = vt->cur_bg;
        vt->buf->saved_flags = vt->cur_flags;
        vt->buf->saved_charset[0] = vt->charset[0];
        vt->buf->saved_charset[1] = vt->charset[1];
        break;
    case '8': {
        VtBuffer *b = vt->buf;
        vt->cur_fg = b->saved_fg;
        vt->cur_bg = b->saved_bg;
        vt->cur_flags = b->saved_flags;
        vt->charset[0] = b->saved_charset[0];
        vt->charset[1] = b->saved_charset[1];
        vt_cursor_set(vt, b->saved_cx, b->saved_cy);
        break;
    }
    case '=': vt->appkeypad = true; break;
    case '>': vt->appkeypad = false; break;
    case 'c': vt_do_reset(vt, true); break;
    case '\\': break;
    default: break;
    }
}

/* ---------------------------------------------------------------- */
/* OSC / DCS                                                        */
/* ---------------------------------------------------------------- */
static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static char *b64decode(const char *s, size_t len, size_t *outlen)
{
    char *out = (char *)malloc(len + 4);
    size_t n = 0;
    int acc = 0, bits = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '=') break;
        int v = b64val((unsigned char)s[i]);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (char)((acc >> bits) & 0xFF);
        }
    }
    out[n] = 0;
    if (outlen) *outlen = n;
    return out;
}

static bool parse_color_spec(const char *s, size_t len, uint32_t *out)
{
    if (len < 4 || s[0] != '#') return false;
    unsigned r = 0, g = 0, b = 0;
    if (len == 7) {
        if (sscanf(s + 1, "%2x%2x%2x", &r, &g, &b) != 3) return false;
    } else if (len == 4) {
        unsigned r1 = 0, g1 = 0, b1 = 0;
        if (sscanf(s + 1, "%1x%1x%1x", &r1, &g1, &b1) != 3) return false;
        r = r1 * 17; g = g1 * 17; b = b1 * 17;
    } else {
        return false;
    }
    *out = VT_COLOR_RGB(r, g, b);
    return true;
}

static void vt_osc_dispatch(Vt *vt, const char *s, size_t len)
{
    if (len == 0) return;
    size_t i = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') i++;
    int code = 0;
    for (size_t k = 0; k < i && k < 6; k++) code = code * 10 + (s[k] - '0');
    const char *rest = s + i;
    size_t rlen = len - i;
    if (rlen > 0 && *rest == ';') { rest++; rlen--; }
    switch (code) {
    case 0: case 1: case 2: {
        size_t n = rlen < sizeof(vt->title) - 1 ? rlen : sizeof(vt->title) - 1;
        memcpy(vt->title, rest, n);
        vt->title[n] = 0;
        if (vt->title_cb) vt->title_cb(vt->ud, vt->title);
        break;
    }
    case 4: {
        /* 4;idx;spec  /  4;idx;? */
        const char *p = rest;
        size_t rem = rlen;
        while (rem > 0) {
            char *end = NULL;
            long idx = strtol(p, &end, 10);
            if (!end || end == p) break;
            size_t used = (size_t)(end - p);
            if (used >= rem) break;
            p += used; rem -= used;
            if (*p == ';') { p++; rem--; }
            const char *semi = memchr(p, ';', rem);
            size_t speclen = semi ? (size_t)(semi - p) : rem;
            uint32_t c;
            if (speclen == 1 && p[0] == '?' && idx >= 0 && idx < 256) {
                c = vt->palette[idx];
                vt_respond(vt, "\x1b]4;%ld;#%06x\x1b\\", idx, c & 0xFFFFFFu);
            } else if (idx >= 0 && idx < 256 && parse_color_spec(p, speclen, &c)) {
                vt->palette[idx] = c;
                vt->palette_custom[idx] = true;
            }
            if (!semi) break;
            rem -= speclen + 1;
            p = semi + 1;
        }
        break;
    }
    case 7: break;                              /* 当前目录，先不处理 */
    case 8: break;                              /* 超链接 */
    case 9:                                     /* iTerm2 通知 */
    case 777: {
        if (code == 777) {
            const char *p = memchr(rest, ';', rlen);
            if (!p) break;
            size_t used = (size_t)(p - rest);
            if (used < 6 || strncmp(rest, "notify", 6) != 0) break;
            rest = p + 1; rlen -= used + 1;
        }
        const char *semi = memchr(rest, ';', rlen);
        char *t = NULL, *body = NULL;
        if (code == 9 && semi) {
            t = strndup(rest, (size_t)(semi - rest));
            body = strndup(semi + 1, rlen - (size_t)(semi - rest) - 1);
        } else {
            body = strndup(rest, rlen);
        }
        if (vt->notify_cb) vt->notify_cb(vt->ud, t ? t : "", body ? body : "");
        free(t);
        free(body);
        break;
    }
    case 10: case 11: case 12: {
        uint32_t *slot = code == 10 ? &vt->osc_fg : (code == 11 ? &vt->osc_bg : &vt->osc_cursor);
        uint32_t fallback = code == 10 ? VT_COLOR_RGB(229, 229, 229)
                          : (code == 11 ? VT_COLOR_RGB(0, 0, 0) : VT_COLOR_RGB(255, 255, 255));
        uint32_t c;
        if (rlen == 1 && *rest == '?') {
            uint32_t v = (*slot == VT_COLOR_DEFAULT) ? fallback : *slot;
            vt_respond(vt, "\x1b]%d;#%06x\x1b\\", code, v & 0xFFFFFFu);
        } else if (parse_color_spec(rest, rlen, &c)) {
            *slot = c;
            vt_mark_all_dirty(vt);
            if (vt->dirty_cb) vt->dirty_cb(vt->ud);
        }
        break;
    }
    case 52: {
        const char *semi = memchr(rest, ';', rlen);
        if (!semi) break;
        const char *data = semi + 1;
        size_t dlen = rlen - (size_t)(semi - rest) - 1;
        if (dlen == 1 && *data == '?') {
            vt_respond(vt, "\x1b]52;c;\x1b\\");
            break;
        }
        size_t outn = 0;
        char *dec = b64decode(data, dlen, &outn);
        if (dec && outn > 0 && vt->clipboard_cb) vt->clipboard_cb(vt->ud, dec, outn);
        free(dec);
        break;
    }
    case 104:
        if (rlen == 0) {
            for (int k = 0; k < 256; k++) { vt->palette[k] = vt_default_palette_entry(k); vt->palette_custom[k] = false; }
        }
        vt_mark_all_dirty(vt);
        break;
    case 110: vt->osc_fg = VT_COLOR_DEFAULT; vt_mark_all_dirty(vt); break;
    case 111: vt->osc_bg = VT_COLOR_DEFAULT; vt_mark_all_dirty(vt); break;
    case 112: vt->osc_cursor = VT_COLOR_DEFAULT; vt_mark_all_dirty(vt); break;
    case 133: {
        /* shell 集成：A=提示符开始 B=命令开始 C=输出开始 D=命令结束 */
        if (rlen > 0 && rest[0] == 'A') {
            VtLine *l = vt_screen_line(vt, vt->buf->cy);
            if (l) l->flags |= VT_LINE_PROMPT;
            if (vt->prompt_cb) vt->prompt_cb(vt->ud, vt->buf->cy);
        }
        break;
    }
    default: break;
    }
}

static void vt_dcs_respond_termcap(Vt *vt, const char *name)
{
    /* tmux 会问 TN/Co 之类，回一个十六进制编码的值 */
    if (strcmp(name, "544e") == 0)                        /* TN = xterm-256color */
        vt_respond(vt, "\x1bP1+r544e=%s\x1b\\", "787465726d2d323536636f6c6f72");
    else if (strcmp(name, "436f") == 0)                   /* Co = 256 */
        vt_respond(vt, "\x1bP1+r436f=%s\x1b\\", "323536");
}

static void vt_dcs_unhook(Vt *vt)
{
    vt->dcs[vt->dcs_len < (int)sizeof(vt->dcs) ? vt->dcs_len : (int)sizeof(vt->dcs) - 1] = 0;
    const char *p = vt->dcs;
    char interm = vt->ninterms > 0 ? vt->interms[0] : 0;
    if (interm == '$' && vt->dcs_final == 'q') {
        const char *name = p;
        if (strcmp(name, "m") == 0) {
            char buf[128];
            int n = snprintf(buf, sizeof(buf), "0%s",
                             (vt->cur_flags & VT_BOLD) ? ";1" : "");
            (void)n;
            vt_respond(vt, "\x1bP1$r%sm\x1b\\", buf);
        } else if (strcmp(name, "r") == 0) {
            vt_respond(vt, "\x1bP1$r%d;%dr\x1b\\", vt->buf->scroll_top + 1, vt->buf->scroll_bot + 1);
        } else if (strcmp(name, "q") == 0) {
            vt_respond(vt, "\x1bP1$r%d q\x1b\\", vt->cursor_style);
        } else {
            vt_respond(vt, "\x1bP0$r\x1b\\");
        }
        return;
    }
    if (interm == '+' && vt->dcs_final == 'q') {
        vt_dcs_respond_termcap(vt, p);
        return;
    }
    if (vt->dcs_final == 't' && strncmp(p, "mux;", 4) == 0 && vt->depth < 4) {
        /* tmux 透传：把 \e\e 还原成 \e 再喂回去 */
        size_t n = (size_t)vt->dcs_len - 4;
        char *out = (char *)malloc(n + 1);
        size_t k = 0;
        for (size_t i = 4; i < (size_t)vt->dcs_len; i++) {
            if (p[i] == 0x1B && i + 1 < (size_t)vt->dcs_len && p[i + 1] == 0x1B) i++;
            out[k++] = p[i];
        }
        vt->depth++;
        vt_input(vt, out, k);
        vt->depth--;
        free(out);
    }
}

/* ---------------------------------------------------------------- */
/* 状态机                                                            */
/* ---------------------------------------------------------------- */
enum {
    S_GROUND = 0, S_ESC, S_ESC_INT, S_CSI_ENTRY, S_CSI_PARAM, S_CSI_INT, S_CSI_IGNORE,
    S_DCS_ENTRY, S_DCS_PARAM, S_DCS_INT, S_DCS_PASS, S_DCS_PASS_ESC, S_DCS_IGNORE,
    S_OSC, S_OSC_ESC, S_SOS, S_SOS_ESC
};

static void vt_parser_reset_seq(Vt *vt)
{
    vt->nparams = 0;
    vt->ninterms = 0;
    vt->csi_private = false;
    for (int i = 0; i < 32; i++) {
        vt->params[i] = 0;
        vt->param_set[i] = false;
        vt->param_colon[i] = false;
    }
}

static void vt_collect(Vt *vt, uint8_t b)
{
    if (vt->ninterms < 3) vt->interms[vt->ninterms++] = (char)b;
}

static void vt_param_action(Vt *vt, uint8_t b)
{
    if (vt->nparams == 0) vt->nparams = 1;
    if (b == ':' || b == ';') {
        if (vt->nparams < 32) {
            int i = vt->nparams++;
            vt->params[i] = 0;
            vt->param_set[i] = false;
            vt->param_colon[i] = (b == ':');
        }
        return;
    }
    int i = vt->nparams - 1;
    vt->params[i] = vt->params[i] * 10 + (b - '0');
    if (vt->params[i] > 65535) vt->params[i] = 65535;
    vt->param_set[i] = true;
}

static int vt_utf8_need(uint8_t b)
{
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;
}

static void vt_execute(Vt *vt, uint8_t b)
{
    VtBuffer *buf = vt->buf;
    switch (b) {
    case 0x07:
        vt->bell_count++;
        if (vt->bell_cb) vt->bell_cb(vt->ud);
        break;
    case 0x08:
        if (buf->cx > 0) vt_cursor_set(vt, buf->cx - 1, buf->cy);
        else if (buf->cy > 0 && (buf->lines[buf->cy - 1].flags & VT_LINE_WRAPPED))
            vt_cursor_set(vt, vt->cols - 1, buf->cy - 1);
        else vt_cursor_set(vt, 0, buf->cy);
        break;
    case 0x09: vt_tab_forward(vt, 1); break;
    case 0x0A: case 0x0B: case 0x0C: {
        int x = vt->newline_mode ? 0 : buf->cx;
        vt_linefeed(vt);
        buf->cx = x;
        break;
    }
    case 0x0D: vt_cursor_set(vt, 0, buf->cy); break;
    case 0x0E: vt->charset_shift = 1; break;
    case 0x0F: vt->charset_shift = 0; break;
    case 0x18: case 0x1A: break;
    default: break;
    }
}

static void vt_ground_byte(Vt *vt, uint8_t b)
{
    if (b == 0x1B) { vt->pstate = S_ESC; vt_parser_reset_seq(vt); return; }
    if (b < 0x20) { vt_execute(vt, b); return; }
    if (b == 0x7F) return;
    vt->u8buf[vt->u8n++] = b;
    int need = vt_utf8_need(vt->u8buf[0]);
    if (vt->u8n < need) return;
    uint32_t cp = 0;
    int used = vt_u8_decode(vt->u8buf, (size_t)vt->u8n, &cp);
    if (used < 1) used = 1;
    if (used > vt->u8n) used = vt->u8n;
    int rest = vt->u8n - used;
    uint8_t tail[4];
    for (int i = 0; i < rest; i++) tail[i] = vt->u8buf[used + i];
    vt->u8n = 0;
    vt_put_char(vt, vt_map_charset(vt, cp));
    for (int i = 0; i < rest; i++) vt_ground_byte(vt, tail[i]);
}

static void vt_dcs_put(Vt *vt, uint8_t b)
{
    if (vt->dcs_len < (int)sizeof(vt->dcs) - 1) vt->dcs[vt->dcs_len++] = (char)b;
}

static void vt_feed_byte(Vt *vt, uint8_t b)
{
    switch (vt->pstate) {
    case S_GROUND:
        vt_ground_byte(vt, b);
        break;
    case S_ESC:
        if (b == 0x1B) { vt_parser_reset_seq(vt); break; }
        if (b < 0x20) { vt_execute(vt, b); break; }
        if (b >= 0x20 && b <= 0x2F) { vt_collect(vt, b); vt->pstate = S_ESC_INT; break; }
        if (b == 0x50) { vt_parser_reset_seq(vt); vt->pstate = S_DCS_ENTRY; break; }
        if (b == 0x58 || b == 0x5E || b == 0x5F) { vt->pstate = S_SOS; break; }
        if (b == 0x5B) { vt_parser_reset_seq(vt); vt->pstate = S_CSI_ENTRY; break; }
        if (b == 0x5D) { vt->osc_len = 0; vt->pstate = S_OSC; break; }
        if (b == 0x7F) break;
        vt_esc_dispatch(vt, b);
        vt->pstate = S_GROUND;
        break;
    case S_ESC_INT:
        if (b == 0x1B) { vt_parser_reset_seq(vt); vt->pstate = S_ESC; break; }
        if (b < 0x20) { vt_execute(vt, b); break; }
        if (b >= 0x20 && b <= 0x2F) { vt_collect(vt, b); break; }
        if (b == 0x7F) break;
        vt_esc_dispatch(vt, b);
        vt->pstate = S_GROUND;
        break;
    case S_CSI_ENTRY:
        if (b == 0x1B) { vt_parser_reset_seq(vt); vt->pstate = S_ESC; break; }
        if (b < 0x20) { vt_execute(vt, b); break; }
        if (b >= 0x40 && b <= 0x7E) { vt_csi_dispatch(vt, b); vt->pstate = S_GROUND; break; }
        if (b >= 0x20 && b <= 0x2F) { vt_collect(vt, b); vt->pstate = S_CSI_INT; break; }
        if ((b >= 0x30 && b <= 0x3B)) { vt_param_action(vt, b); vt->pstate = S_CSI_PARAM; break; }
        if (b >= 0x3C && b <= 0x3F) { if (b == '?') vt->csi_private = true; vt->pstate = S_CSI_PARAM; break; }
        break;
    case S_CSI_PARAM:
        if (b == 0x1B) { vt_parser_reset_seq(vt); vt->pstate = S_ESC; break; }
        if (b < 0x20) { vt_execute(vt, b); break; }
        if (b >= 0x30 && b <= 0x3B) { vt_param_action(vt, b); break; }
        if (b >= 0x20 && b <= 0x2F) { vt_collect(vt, b); vt->pstate = S_CSI_INT; break; }
        if (b >= 0x40 && b <= 0x7E) { vt_csi_dispatch(vt, b); vt->pstate = S_GROUND; break; }
        if (b >= 0x3C && b <= 0x3F) { vt->pstate = S_CSI_IGNORE; break; }
        break;
    case S_CSI_INT:
        if (b == 0x1B) { vt_parser_reset_seq(vt); vt->pstate = S_ESC; break; }
        if (b < 0x20) { vt_execute(vt, b); break; }
        if (b >= 0x20 && b <= 0x2F) { vt_collect(vt, b); break; }
        if (b >= 0x40 && b <= 0x7E) { vt_csi_dispatch(vt, b); vt->pstate = S_GROUND; break; }
        if (b >= 0x30 && b <= 0x3F) { vt->pstate = S_CSI_IGNORE; break; }
        break;
    case S_CSI_IGNORE:
        if (b == 0x1B) { vt_parser_reset_seq(vt); vt->pstate = S_ESC; break; }
        if (b < 0x20) { vt_execute(vt, b); break; }
        if (b >= 0x40 && b <= 0x7E) vt->pstate = S_GROUND;
        break;
    case S_DCS_ENTRY:
        if (b == 0x1B) { vt_parser_reset_seq(vt); vt->pstate = S_ESC; break; }
        if (b < 0x20) { vt_execute(vt, b); break; }
        if (b >= 0x40 && b <= 0x7E) { vt->dcs_len = 0; vt->dcs_final = b; vt->pstate = S_DCS_PASS; break; }
        if (b >= 0x20 && b <= 0x2F) { vt_collect(vt, b); vt->pstate = S_DCS_INT; break; }
        if (b >= 0x30 && b <= 0x3B) { vt_param_action(vt, b); vt->pstate = S_DCS_PARAM; break; }
        if (b >= 0x3C && b <= 0x3F) { vt->pstate = S_DCS_PARAM; break; }
        break;
    case S_DCS_PARAM:
        if (b == 0x1B) { vt_parser_reset_seq(vt); vt->pstate = S_ESC; break; }
        if (b < 0x20) { vt_execute(vt, b); break; }
        if (b >= 0x30 && b <= 0x3B) { vt_param_action(vt, b); break; }
        if (b >= 0x20 && b <= 0x2F) { vt_collect(vt, b); vt->pstate = S_DCS_INT; break; }
        if (b >= 0x40 && b <= 0x7E) { vt->dcs_len = 0; vt->dcs_final = b; vt->pstate = S_DCS_PASS; break; }
        if (b >= 0x3C && b <= 0x3F) { vt->pstate = S_DCS_IGNORE; break; }
        break;
    case S_DCS_INT:
        if (b == 0x1B) { vt_parser_reset_seq(vt); vt->pstate = S_ESC; break; }
        if (b < 0x20) { vt_execute(vt, b); break; }
        if (b >= 0x20 && b <= 0x2F) { vt_collect(vt, b); break; }
        if (b >= 0x40 && b <= 0x7E) { vt->dcs_len = 0; vt->dcs_final = b; vt->pstate = S_DCS_PASS; break; }
        if (b >= 0x30 && b <= 0x3F) { vt->pstate = S_DCS_IGNORE; break; }
        break;
    case S_DCS_IGNORE:
        if (b == 0x1B) { vt_parser_reset_seq(vt); vt->pstate = S_ESC; break; }
        if (b >= 0x40 && b <= 0x7E) vt->pstate = S_GROUND;
        break;
    case S_DCS_PASS:
        if (b == 0x1B) { vt->pstate = S_DCS_PASS_ESC; break; }
        if (b == 0x18 || b == 0x1A) { vt->pstate = S_GROUND; vt_dcs_unhook(vt); break; }
        vt_dcs_put(vt, b);
        break;
    case S_DCS_PASS_ESC:
        if (b == '\\') { vt->pstate = S_GROUND; vt_dcs_unhook(vt); break; }
        vt_dcs_put(vt, 0x1B);
        vt_dcs_put(vt, b);
        vt->pstate = S_DCS_PASS;
        break;
    case S_OSC:
        if (b == 0x07) { vt_osc_dispatch(vt, vt->osc, (size_t)vt->osc_len); vt->pstate = S_GROUND; break; }
        if (b == 0x1B) { vt->pstate = S_OSC_ESC; break; }
        if (b == 0x18 || b == 0x1A) { vt->pstate = S_GROUND; break; }
        if (b < 0x20) break;
        if (vt->osc_len < (int)sizeof(vt->osc) - 1) vt->osc[vt->osc_len++] = (char)b;
        break;
    case S_OSC_ESC:
        if (b == '\\') { vt_osc_dispatch(vt, vt->osc, (size_t)vt->osc_len); vt->pstate = S_GROUND; break; }
        if (vt->osc_len < (int)sizeof(vt->osc) - 1) vt->osc[vt->osc_len++] = 0x1B;
        vt->pstate = S_OSC;
        vt_feed_byte(vt, b);
        break;
    case S_SOS:
        if (b == 0x07) { vt->pstate = S_GROUND; break; }
        if (b == 0x1B) { vt->pstate = S_SOS_ESC; break; }
        if (b == 0x18 || b == 0x1A) vt->pstate = S_GROUND;
        break;
    case S_SOS_ESC:
        if (b == '\\') vt->pstate = S_GROUND;
        else vt->pstate = S_SOS;
        break;
    default:
        vt->pstate = S_GROUND;
        break;
    }
}

void vt_input(Vt *vt, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) vt_feed_byte(vt, p[i]);
}

/* ---------------------------------------------------------------- */
/* 尺寸变化 + 重新折行                                                */
/* ---------------------------------------------------------------- */
#define VT_REFLOW_MAX 800

typedef struct {
    VtCell *cells;
    uint8_t *hard;
    int      n, cap;
} VtFlat;

static void vt_flat_push(VtFlat *f, VtCell c, int hard)
{
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 4096;
        f->cells = (VtCell *)realloc(f->cells, sizeof(VtCell) * (size_t)f->cap);
        f->hard = (uint8_t *)realloc(f->hard, (size_t)f->cap);
    }
    f->cells[f->n] = c;
    f->hard[f->n] = (uint8_t)hard;
    f->n++;
}

static int vt_cell_is_blank(const VtCell *c)
{
    return c->cp == ' ' && c->cp2 == 0 && c->cp3 == 0 &&
           c->fg == VT_COLOR_DEFAULT && c->bg == VT_COLOR_DEFAULT &&
           (c->flags & (uint16_t)~VT_WIDE_PAD) == 0;
}

void vt_resize(Vt *vt, int cols, int rows)
{
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols == vt->cols && rows == vt->rows) return;

    int oldlimit = vt->sb_limit;
    (void)oldlimit;

    VtBuffer *nb = &vt->normal;
    int keep = vt->sb_len > VT_REFLOW_MAX ? vt->sb_len - VT_REFLOW_MAX : 0;
    for (int i = keep; i < vt->sb_len; i++) {
        VtLine *l = vt_sb_line(vt, i);
        if (l->flags & VT_LINE_KEEP) { keep = i; break; }
    }
    VtFlat flat;
    memset(&flat, 0, sizeof(flat));
    int cursor_flat = -1;
    /* 光标所在的这一行是不是上一行折过来的（决定光标是"接在行尾"还是"在新行开头"） */
    int cursor_continues = (nb->cy > 0 && (nb->lines[nb->cy - 1].flags & VT_LINE_WRAPPED)) ? 1 : 0;
    /* 屏幕底部没内容的空行不参与重排，否则会把内容顶下去 */
    int last_rel = nb->cy;
    for (int y = nb->rows - 1; y >= 0; y--) {
        VtLine *l = &nb->lines[y];
        int nonblank = 0;
        for (int x = 0; x < l->cols; x++) {
            if (!vt_cell_is_blank(&l->cells[x])) { nonblank = 1; break; }
        }
        if (nonblank) {
            if (y > last_rel) last_rel = y;
            break;
        }
    }
    int total_rows = (vt->sb_len - keep) + last_rel + 1;
    for (int r = 0; r < total_rows; r++) {
        VtLine *l = (r < vt->sb_len - keep) ? vt_sb_line(vt, keep + r) : &nb->lines[r - (vt->sb_len - keep)];
        int base = flat.n;
        if (r == total_rows - 1 && !(l->flags & VT_LINE_WRAPPED)) { /* 最后一行不裁 */ }
        int hard = !(l->flags & VT_LINE_WRAPPED);
        int last = l->cols;
        if (hard) {
            while (last > 0 && vt_cell_is_blank(&l->cells[last - 1])) last--;
        }
        /* 硬换行的标记要挂在最后一个真实格子上：宽字符的续格是零宽的，
           挂上去会被下面的重排循环跳过，换行就丢了（两行会被并成一行）。 */
        int hard_at = -1;
        for (int x = 0; x < last; x++) {
            if (l->cells[x].flags & VT_WIDE_PAD) continue;   /* 填充格不算内容 */
            vt_flat_push(&flat, l->cells[x], 0);
            if (l->cells[x].width != 0) hard_at = flat.n - 1;
        }
        if (hard) {
            if (hard_at >= 0) flat.hard[hard_at] = 1;
            else vt_flat_push(&flat, vt_blank_cell(VT_COLOR_DEFAULT), 1);
        }
        if (r == (vt->sb_len - keep) + nb->cy) {
            int cx = nb->cx + (nb->wrapped ? 1 : 0);
            if (cx > last) cx = last;
            cursor_flat = base + cx;
        }
    }
    for (int i = keep; i < vt->sb_len; i++) vt_line_free(vt_sb_line(vt, i));
    vt->sb_len = keep;

    /* 旧回滚行按新宽度裁剪/补齐 */
    for (int i = 0; i < vt->sb_len; i++) {
        VtLine *l = vt_sb_line(vt, i);
        vt_line_refit(l, cols, VT_COLOR_DEFAULT);
    }
    for (int y = 0; y < nb->rows; y++) vt_line_free(&nb->lines[y]);
    free(nb->lines);
    nb->lines = NULL;

    /* 重新按新宽度折行 */
    int cap = 64, n = 0;
    VtLine *out = (VtLine *)malloc(sizeof(VtLine) * (size_t)cap);
    VtLine cur = vt_line_new(cols, VT_COLOR_DEFAULT);
    int pos = 0, cursor_out = -1, cursor_pos = 0, last_emit_pos = 0;
    for (int i = 0; i < flat.n; i++) {
        VtCell c = flat.cells[i];
        if (c.width == 0) continue;   /* 宽字符占位格 */
        int w = c.width ? c.width : 1;
        if (i == cursor_flat) { cursor_out = n; cursor_pos = pos; }
        if (pos + w > cols) {
            if (n == cap) { cap *= 2; out = (VtLine *)realloc(out, sizeof(VtLine) * (size_t)cap); }
            cur.flags = VT_LINE_WRAPPED;
            for (int k = pos; k < cols; k++) cur.cells[k].flags |= VT_WIDE_PAD;
            last_emit_pos = pos;
            out[n++] = cur;
            cur = vt_line_new(cols, VT_COLOR_DEFAULT);
            pos = 0;
            if (i == cursor_flat) { cursor_out = n; cursor_pos = 0; }
        }
        cur.cells[pos] = c;
        if (w == 2 && pos + 1 < cols) {
            cur.cells[pos + 1] = c;
            cur.cells[pos + 1].cp = 0;
            cur.cells[pos + 1].cp2 = 0;
            cur.cells[pos + 1].cp3 = 0;
            cur.cells[pos + 1].width = 0;
            cur.cells[pos + 1].flags = (uint16_t)(c.flags | VT_WIDE_CONT);
        }
        pos += w;
        if (flat.hard[i]) {
            cur.flags = 0;
            last_emit_pos = pos;
            if (n == cap) { cap *= 2; out = (VtLine *)realloc(out, sizeof(VtLine) * (size_t)cap); }
            out[n++] = cur;
            cur = vt_line_new(cols, VT_COLOR_DEFAULT);
            pos = 0;
            if (i + 1 == cursor_flat && !(cursor_continues && i + 1 == flat.n)) {
                cursor_out = n;
                cursor_pos = 0;
            }
        }
    }
    if (pos > 0 || n == 0) {
        if (n == cap) { cap *= 2; out = (VtLine *)realloc(out, sizeof(VtLine) * (size_t)cap); }
        if (cursor_flat == flat.n && cursor_continues) { cursor_out = n; cursor_pos = pos; }
        out[n++] = cur;
        cur.cells = NULL;
    } else if (cursor_flat == flat.n && cursor_continues) {
        cursor_out = n - 1;
        cursor_pos = last_emit_pos;
    }
    if (cur.cells) vt_line_free(&cur);

    /* 尾巴 rows 行进屏幕，前面的进回滚区 */
    int screen_start = n - rows;
    if (screen_start < 0) screen_start = 0;
    for (int i = 0; i < screen_start; i++) vt_sb_push(vt, out[i]);
    int have = n - screen_start;
    nb->lines = (VtLine *)calloc((size_t)rows, sizeof(VtLine));
    for (int y = 0; y < rows; y++) {
        if (y < have) nb->lines[y] = out[screen_start + y];
        else nb->lines[y] = vt_line_new(cols, VT_COLOR_DEFAULT);
    }
    free(out);
    free(flat.cells);
    free(flat.hard);

    /* 光标 */
    int cy = cursor_out >= 0 ? cursor_out - screen_start : rows - 1;
    if (cy < 0) cy = 0;
    if (cy > rows - 1) cy = rows - 1;
    int cx = cursor_pos;
    int pending_wrap = 0;
    if (cx > cols - 1) { cx = cols - 1; pending_wrap = 1; }

    /* 备用屏：不重排，直接裁剪 */
    for (int y = 0; y < vt->alt.rows; y++) vt_line_free(&vt->alt.lines[y]);
    free(vt->alt.lines);
    vt->alt.lines = (VtLine *)calloc((size_t)rows, sizeof(VtLine));
    for (int y = 0; y < rows; y++) vt->alt.lines[y] = vt_line_new(cols, VT_COLOR_DEFAULT);
    vt->alt.cols = cols;
    vt->alt.rows = rows;
    vt->alt.cx = vt->alt.cx < cols ? vt->alt.cx : cols - 1;
    vt->alt.cy = vt->alt.cy < rows ? vt->alt.cy : rows - 1;
    vt->alt.scroll_top = 0;
    vt->alt.scroll_bot = rows - 1;

    nb->cols = cols;
    nb->rows = rows;
    nb->cx = cx;
    nb->cy = cy;
    nb->wrapped = pending_wrap;
    nb->scroll_top = 0;
    nb->scroll_bot = rows - 1;
    nb->saved_cx = nb->saved_cx < cols ? nb->saved_cx : cols - 1;
    nb->saved_cy = nb->saved_cy < rows ? nb->saved_cy : rows - 1;

    vt->cols = cols;
    vt->rows = rows;
    free(vt->tabs);
    vt->tabs = (uint8_t *)malloc((size_t)cols);
    for (int i = 0; i < cols; i++) vt->tabs[i] = ((i % 8) == 0);

    vt_mark_all_dirty(vt);
    vt->cursor_dirty++;
}

/* ---------------------------------------------------------------- */
/* 取文本                                                            */
/* ---------------------------------------------------------------- */
static void sb_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap) *cap = *cap ? *cap * 2 : 256;
        *buf = (char *)realloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

static void sb_append_cp(char **buf, size_t *len, size_t *cap, uint32_t cp)
{
    if (cp == 0) return;
    uint8_t u[4];
    int n = vt_u8_encode(cp, u);
    sb_append(buf, len, cap, (const char *)u, (size_t)n);
}

static void line_text_into(VtLine *l, int x0, int x1, char **buf, size_t *len, size_t *cap, bool trim)
{
    if (x1 > l->cols) x1 = l->cols;
    if (trim) {
        while (x1 > x0 && vt_cell_is_blank(&l->cells[x1 - 1])) x1--;
    }
    for (int x = x0; x < x1; x++) {
        VtCell *c = &l->cells[x];
        if (c->width == 0) continue;
        if (c->flags & VT_WIDE_PAD) continue;
        sb_append_cp(buf, len, cap, c->cp);
        sb_append_cp(buf, len, cap, c->cp2);
        sb_append_cp(buf, len, cap, c->cp3);
    }
}

char *vt_row_text(Vt *vt, int row, bool trim_right)
{
    VtLine *l = vt_line_at(vt, row);
    if (!l) return strdup("");
    char *buf = NULL;
    size_t len = 0, cap = 0;
    sb_append(&buf, &len, &cap, "", 0);
    line_text_into(l, 0, l->cols, &buf, &len, &cap, trim_right);
    return buf ? buf : strdup("");
}

char *vt_range_text(Vt *vt, int r0, int c0, int r1, int c1)
{
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        int t;
        t = r0; r0 = r1; r1 = t;
        t = c0; c0 = c1; c1 = t;
    }
    char *buf = NULL;
    size_t len = 0, cap = 0;
    sb_append(&buf, &len, &cap, "", 0);
    for (int r = r0; r <= r1; r++) {
        VtLine *l = vt_line_at(vt, r);
        if (!l) continue;
        int x0 = (r == r0) ? c0 : 0;
        int x1 = (r == r1) ? c1 + 1 : l->cols;
        line_text_into(l, x0, x1, &buf, &len, &cap, true);
        if (r != r1 && !(l->flags & VT_LINE_WRAPPED))
            sb_append(&buf, &len, &cap, "\n", 1);
    }
    return buf ? buf : strdup("");
}

/* ---------------------------------------------------------------- */
/* 键盘编码                                                          */
/* ---------------------------------------------------------------- */
static void app_key(char *out, size_t outsz, int *n, const char *app, const char *norm)
{
    const char *s = app ? app : norm;
    size_t l = strlen(s);
    if ((size_t)*n + l >= outsz) l = outsz - (size_t)*n - 1;
    memcpy(out + *n, s, l);
    *n += (int)l;
    out[*n] = 0;
}

int vt_key_encode(Vt *vt, int key, int mods, uint32_t ch, char *out, size_t outsz)
{
    int n = 0;
    if (outsz == 0) return 0;
    out[0] = 0;
    int mod = 1 + ((mods & VTM_SHIFT) ? 1 : 0) + ((mods & VTM_ALT) ? 2 : 0) + ((mods & VTM_CTRL) ? 4 : 0);
    bool has_mod = (mod > 1);
    char seq[32];

    switch (key) {
    case VK_UP: case VK_DOWN: case VK_RIGHT: case VK_LEFT: {
        char f = key == VK_UP ? 'A' : key == VK_DOWN ? 'B' : key == VK_RIGHT ? 'C' : 'D';
        if (has_mod) snprintf(seq, sizeof(seq), "\x1b[1;%d%c", mod, f);
        else if (vt->appcursor) snprintf(seq, sizeof(seq), "\x1bO%c", f);
        else snprintf(seq, sizeof(seq), "\x1b[%c", f);
        app_key(out, outsz, &n, seq, seq);
        return n;
    }
    case VK_HOME: case VK_END: {
        char f = key == VK_HOME ? 'H' : 'F';
        if (has_mod) snprintf(seq, sizeof(seq), "\x1b[1;%d%c", mod, f);
        else if (vt->appcursor) snprintf(seq, sizeof(seq), "\x1bO%c", f);
        else snprintf(seq, sizeof(seq), "\x1b[%c", f);
        app_key(out, outsz, &n, seq, seq);
        return n;
    }
    case VK_INSERT:
        if (has_mod) snprintf(seq, sizeof(seq), "\x1b[2;%d~", mod);
        else snprintf(seq, sizeof(seq), "\x1b[2~");
        app_key(out, outsz, &n, seq, seq);
        return n;
    case VK_DELETE:
        if (has_mod) snprintf(seq, sizeof(seq), "\x1b[3;%d~", mod);
        else snprintf(seq, sizeof(seq), "\x1b[3~");
        app_key(out, outsz, &n, seq, seq);
        return n;
    case VK_PAGEUP: case VK_PAGEDOWN: {
        int c = key == VK_PAGEUP ? 5 : 6;
        if (has_mod) snprintf(seq, sizeof(seq), "\x1b[%d;%d~", c, mod);
        else snprintf(seq, sizeof(seq), "\x1b[%d~", c);
        app_key(out, outsz, &n, seq, seq);
        return n;
    }
    case VK_BACKTAB: snprintf(seq, sizeof(seq), "\x1b[Z"); app_key(out, outsz, &n, seq, seq); return n;
    case VK_F1: case VK_F2: case VK_F3: case VK_F4: {
        char f = (char)('P' + (key - VK_F1));
        snprintf(seq, sizeof(seq), "\x1bO%c", f);
        app_key(out, outsz, &n, seq, seq);
        return n;
    }
    case VK_F5: case VK_F6: case VK_F7: case VK_F8:
        snprintf(seq, sizeof(seq), "\x1b[%d~", 15 + (key - VK_F5));
        app_key(out, outsz, &n, seq, seq); return n;
    case VK_F9: case VK_F10: case VK_F11: case VK_F12:
        snprintf(seq, sizeof(seq), "\x1b[%d~", 20 + (key - VK_F9));
        app_key(out, outsz, &n, seq, seq); return n;
    case VK_ESC: out[n++] = 0x1B; out[n] = 0; return n;
    case VK_TAB:
        if (mods & VTM_SHIFT) { memcpy(out, "\x1b[Z", 3); return 3; }
        out[n++] = 0x09; out[n] = 0; return n;
    case VK_ENTER:
        if (mods & VTM_ALT) { out[n++] = 0x1B; }
        out[n++] = (vt->newline_mode ? 0x0A : 0x0D);
        out[n] = 0; return n;
    case VK_BACKSPACE:
        if (mods & VTM_ALT) out[n++] = 0x1B;
        out[n++] = (mods & VTM_CTRL) ? 0x08 : 0x7F;
        out[n] = 0; return n;
    case VK_CHAR:
    default: break;
    }

    if (ch == 0) return 0;
    if (mods & VTM_CTRL) {
        uint32_t c = 0;
        if (ch >= 'a' && ch <= 'z') c = ch - 'a' + 1;
        else if (ch >= 'A' && ch <= 'Z') c = ch - 'A' + 1;
        else if (ch == ' ' || ch == '@' || ch == '2') c = 0;
        else if (ch >= '[' && ch <= '_') c = ch - '[' + 27;
        else if (ch >= '3' && ch <= '7') c = ch - '3' + 27;
        else if (ch == '8' || ch == '?') c = 0x7F;
        else if (ch == '9') c = 0;
        else if (ch >= '0' && ch <= '9') c = 0;
        if (c) {
            if (mods & VTM_ALT) out[n++] = 0x1B;
            out[n++] = (char)c;
            out[n] = 0;
            return n;
        }
    }
    if (mods & VTM_ALT) out[n++] = 0x1B;
    {
        uint8_t u[4];
        int l = vt_u8_encode(ch, u);
        if ((size_t)n + (size_t)l >= outsz) l = (int)(outsz - (size_t)n - 1);
        if (l > 0) memcpy(out + n, u, (size_t)l);
        n += l;
        out[n] = 0;
    }
    return n;
}
