#import "TerminalView.h"
#import "TerminalSession.h"
#import "vt_unicode.h"
#import <CoreText/CoreText.h>
#import <QuartzCore/QuartzCore.h>
#import <math.h>
#import <stdlib.h>
#import <string.h>

/* 这个 View 明确要兼容 iOS 12，用到 UIMenuController 这类"新系统里过时"的 API 是故意的 */
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* ---------------- UITextInput 用的替身对象 ---------------- */
@interface TermPosition : UITextPosition
@property (nonatomic) NSInteger offset;
+ (instancetype)pos:(NSInteger)o;
@end

@implementation TermPosition
+ (instancetype)pos:(NSInteger)o
{
    TermPosition *p = [[TermPosition alloc] init];
    p.offset = o;
    return p;
}
@end

@interface TermRange : UITextRange
@property (nonatomic, strong) TermPosition *startPos;
@property (nonatomic, strong) TermPosition *endPos;
@end

@implementation TermRange
- (UITextPosition *)start { return self.startPos; }
- (UITextPosition *)end { return self.endPos; }
- (BOOL)isEmpty { return self.startPos.offset == self.endPos.offset; }
@end

@interface TermSelectionRect : UITextSelectionRect
@property (nonatomic) CGRect rectValue;
@end

@implementation TermSelectionRect
- (CGRect)rect { return self.rectValue; }
- (UITextWritingDirection)writingDirection { return UITextWritingDirectionLeftToRight; }
- (BOOL)containsStart { return YES; }
- (BOOL)containsEnd { return NO; }
- (BOOL)isVertical { return NO; }
@end

/* ---------------- 行渲染缓存 ---------------- */
typedef struct {
    const void *linePtr;
    uint32_t    rev;
    uint16_t    cols;
    CTLineRef   line;
} TermRowCache;

/* 一段同属性的字，用来算字距对齐 */
typedef struct {
    NSUInteger loc, len;
    int        cells;
    CTFontRef  font;
    uint32_t   fg;
    uint32_t   bg;
    uint16_t   flags;
    int        cluster;   /* 组合字符/非 BMP，只在结尾补一次字距 */
} TermSpan;

static NSString *TermUniCharToString(UTF32Char c)
{
    if (c == 0) return @" ";
    if (c <= 0xFFFF) {
        UniChar u = (UniChar)c;
        return [NSString stringWithCharacters:&u length:1];
    }
    UTF32Char v = c - 0x10000;
    UniChar u[2];
    u[0] = (UniChar)(0xD800 + (v >> 10));
    u[1] = (UniChar)(0xDC00 + (v & 0x3FF));
    return [NSString stringWithCharacters:u length:2];
}

@interface TerminalView () <UIGestureRecognizerDelegate>
@end

@implementation TerminalView {
    CTFontRef   _font, _boldFont, _italicFont, _boldItalicFont;
    CGFloat     _cellW, _cellH, _ascent, _descent;
    int         _cols, _rows;
    TermRowCache *_cache;
    int         _cacheCount;
    NSInteger   _scrollOffset;
    NSTimer    *_blinkTimer;
    BOOL        _cursorOn;
    NSInteger   _lastCursorX, _lastCursorY;
    /* 选区(全局行号) */
    BOOL        _hasSelection;
    NSInteger   _selAnchorRow, _selAnchorCol;
    NSInteger   _selHeadRow, _selHeadCol;
    /* 输入法 */
    NSString   *_markedText;
    NSRange     _markedSel;
    NSDictionary *_markedTextStyle;
    NSMutableDictionary *_colorCache;
    uint32_t    _paletteHash;
    id<UITextInputDelegate> _inputDelegate;
    UITextInputStringTokenizer *_tokenizer;
    CGPoint     _panStartPoint;
    NSInteger   _panStartOffset;
    UITouch     *_selectTouch;
    BOOL        _selectionDrag;
    CGFloat     _pinchBaseSize;
    BOOL        _suppressCaretNav;
    BOOL        _mousePressed;
    NSInteger   _mouseRow, _mouseCol;
}

/* ================= 初始化 ================= */

- (instancetype)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if (self) {
        [self commonInit];
    }
    return self;
}

- (void)commonInit
{
    self.opaque = YES;
    self.multipleTouchEnabled = YES;
    self.contentMode = UIViewContentModeRedraw;
    self.fontSize = 14.0;
    self.fontName = @"Menlo";
    self.cursorBlink = YES;
    self.boldAsBright = YES;
    _cursorOn = YES;
    _colorCache = [NSMutableDictionary dictionary];
    _tokenizer = [[UITextInputStringTokenizer alloc] initWithTextInput:self];
    [self reloadAppearance];

    UIPanGestureRecognizer *pan = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(handlePan:)];
    pan.delegate = self;
    pan.maximumNumberOfTouches = 1;
    [self addGestureRecognizer:pan];

    UIPinchGestureRecognizer *pinch = [[UIPinchGestureRecognizer alloc] initWithTarget:self action:@selector(handlePinch:)];
    [self addGestureRecognizer:pinch];

    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleTap:)];
    [self addGestureRecognizer:tap];

    UITapGestureRecognizer *doubleTap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleDoubleTap:)];
    doubleTap.numberOfTapsRequired = 2;
    [tap requireGestureRecognizerToFail:doubleTap];
    [self addGestureRecognizer:doubleTap];

    UILongPressGestureRecognizer *longPress = [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleLongPress:)];
    longPress.minimumPressDuration = 0.4;
    longPress.delegate = self;
    [self addGestureRecognizer:longPress];

    UITapGestureRecognizer *tripleTap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleTripleTap:)];
    tripleTap.numberOfTapsRequired = 3;
    [doubleTap requireGestureRecognizerToFail:tripleTap];
    [self addGestureRecognizer:tripleTap];

    self.keyboardType = UIKeyboardTypeDefault;
    self.autocapitalizationType = UITextAutocapitalizationTypeNone;
    self.autocorrectionType = UITextAutocorrectionTypeNo;
    self.spellCheckingType = UITextSpellCheckingTypeNo;
    self.enablesReturnKeyAutomatically = NO;
    self.returnKeyType = UIReturnKeyDefault;
    self.secureTextEntry = NO;
    self.confirmMultilinePaste = YES;
    self.bracketPaste = YES;
}

- (void)dealloc
{
    [_blinkTimer invalidate];
    [self invalidateRowCache];
    if (_font) CFRelease(_font);
    if (_boldFont) CFRelease(_boldFont);
    if (_italicFont) CFRelease(_italicFont);
    if (_boldItalicFont) CFRelease(_boldItalicFont);
}

/* ================= 字体 ================= */

- (CTFontRef)createFont:(CTFontSymbolicTraits)traits italic:(BOOL)italic
{
    NSMutableDictionary *attrs = [NSMutableDictionary dictionary];
    attrs[(__bridge NSString *)kCTFontNameAttribute] = self.fontName ?: @"Menlo";
    NSMutableArray *cascade = [NSMutableArray array];
    NSArray *names = @[ @"PingFang SC", @"Hiragino Sans GB", @"Heiti SC", @"Apple Color Emoji", @"Helvetica" ];
    for (NSString *n in names) {
        CTFontDescriptorRef d = CTFontDescriptorCreateWithNameAndSize((__bridge CFStringRef)n, self.fontSize);
        if (d) {
            [cascade addObject:(__bridge id)d];
            CFRelease(d);
        }
    }
    attrs[(__bridge NSString *)kCTFontCascadeListAttribute] = cascade;
    CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes((__bridge CFDictionaryRef)attrs);
    CTFontRef font;
    if (italic) {
        CGAffineTransform m = CGAffineTransformMake(1.0, 0.0, 0.22, 1.0, 0.0, 0.0);
        font = CTFontCreateWithFontDescriptorAndOptions(desc, self.fontSize, &m, 0);
    } else {
        font = CTFontCreateWithFontDescriptor(desc, self.fontSize, NULL);
    }
    CFRelease(desc);
    if (traits) {
        CTFontRef t = CTFontCreateCopyWithSymbolicTraits(font, self.fontSize, NULL, traits, traits);
        if (t) {
            CFRelease(font);
            font = t;
        }
    }
    return font;
}

- (void)reloadAppearance
{
    if (_font) CFRelease(_font);
    if (_boldFont) CFRelease(_boldFont);
    if (_italicFont) CFRelease(_italicFont);
    if (_boldItalicFont) CFRelease(_boldItalicFont);
    _font = [self createFont:0 italic:NO];
    _boldFont = [self createFont:kCTFontTraitBold italic:NO];
    _italicFont = [self createFont:kCTFontTraitItalic italic:YES];
    _boldItalicFont = [self createFont:(kCTFontTraitBold | kCTFontTraitItalic) italic:YES];

    _cellW = [self advanceOfString:@"0" font:_font];
    if (_cellW < 1) _cellW = self.fontSize * 0.6;
    _ascent = ceil(CTFontGetAscent(_font));
    _descent = ceil(CTFontGetDescent(_font));
    _cellH = _ascent + _descent;
    if (_cellH < 1) _cellH = self.fontSize * 1.2;
    uint32_t bg = self.theme->bg;
    CGFloat lum = (((bg >> 16) & 0xFF) * 0.299 + ((bg >> 8) & 0xFF) * 0.587 + (bg & 0xFF) * 0.114) / 255.0;
    self.keyboardAppearance = (lum < 0.5) ? UIKeyboardAppearanceDark : UIKeyboardAppearanceLight;
    [_colorCache removeAllObjects];
    _paletteHash = 0;
    [self invalidateRowCache];
    [self updateGridSize];
    [self setNeedsDisplay];
}

@synthesize theme = _theme;

- (const TermTheme *)theme { return _theme ? _theme : TermThemes(); }

- (void)setTheme:(const TermTheme *)t
{
    _theme = t;
    if (_colorCache) [self reloadAppearance];
}

- (void)setFontSize:(CGFloat)s
{
    if (s < 6) s = 6;
    if (s > 40) s = 40;
    if (fabs(s - _fontSize) < 0.01) return;
    _fontSize = s;
    if (_colorCache) [self reloadAppearance];
}

- (void)setFontName:(NSString *)n
{
    if (!n.length || [n isEqualToString:_fontName]) return;
    _fontName = [n copy];
    if (_colorCache) [self reloadAppearance];
}

- (CGFloat)advanceOfString:(NSString *)s font:(CTFontRef)font
{
    NSDictionary *a = @{ (__bridge NSString *)kCTFontAttributeName: (__bridge id)font };
    NSAttributedString *as = [[NSAttributedString alloc] initWithString:s attributes:a];
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as);
    double w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    CFRelease(line);
    return (CGFloat)w;
}

- (CGFloat)advanceOfAttributed:(NSAttributedString *)as
{
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as);
    double w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    CFRelease(line);
    return (CGFloat)w;
}

/* ================= 尺寸 ================= */

- (int)cols { return _cols; }
- (int)rows { return _rows; }
- (CGFloat)cellWidth { return _cellW; }
- (CGFloat)cellHeight { return _cellH; }

- (void)setFrame:(CGRect)frame
{
    [super setFrame:frame];
    [self updateGridSize];
}

- (void)layoutSubviews
{
    [super layoutSubviews];
    [self updateGridSize];
}

- (void)updateGridSize
{
    if (_cellW <= 0 || _cellH <= 0) return;
    int c = (int)floor(self.bounds.size.width / _cellW);
    int r = (int)floor(self.bounds.size.height / _cellH);
    if (c < 2) c = 2;
    if (r < 2) r = 2;
    if (c == _cols && r == _rows) return;
    _cols = c;
    _rows = r;
    [self invalidateRowCache];
    [self.delegate terminalView:self didResizeCols:c rows:r];
}

- (void)invalidateRowCache
{
    if (_cache) {
        for (int i = 0; i < _cacheCount; i++) {
            if (_cache[i].line) CFRelease(_cache[i].line);
        }
        free(_cache);
        _cache = NULL;
    }
    _cacheCount = 0;
}

- (void)ensureCache
{
    int need = _rows > 0 ? _rows : 1;
    if (_cache && _cacheCount >= need) return;
    [self invalidateRowCache];
    _cacheCount = need;
    _cache = (TermRowCache *)calloc((size_t)_cacheCount, sizeof(TermRowCache));
}

/* ================= 颜色 ================= */

- (void)refreshPaletteIfNeeded
{
    if (!self.vt) return;
    uint32_t h = 0;
    for (int i = 0; i < 256; i++) h = h * 31 + self.vt->palette[i];
    h = h * 31 + self.vt->osc_fg + self.vt->osc_bg * 7;
    if (h != _paletteHash) {
        _paletteHash = h;
        [_colorCache removeAllObjects];
    }
}

- (UIColor *)colorForVT:(uint32_t)c fallback:(uint32_t)fallback
{
    NSNumber *key = @(c);
    UIColor *col = _colorCache[key];
    if (col) return col;
    uint32_t pal[256];
    uint32_t *p = NULL;
    if (self.vt) {
        memcpy(pal, self.vt->palette, sizeof(pal));
        p = pal;
    }
    col = TermColorFor(c, self.theme, fallback, p);
    _colorCache[key] = col;
    return col;
}

- (UIColor *)backgroundColor
{
    return [self colorForVT:0xFFFFFFFFu fallback:self.theme->bg];
}

- (uint32_t)effectiveFgForCell:(const VtCell *)c
{
    uint32_t f = c->fg, b = c->bg;
    BOOL rev = (self.vt && self.vt->reverse_video) ^ ((c->flags & VT_REVERSE) != 0);
    if (rev) { uint32_t t = f; f = b; b = t; }
    if (f == VT_COLOR_DEFAULT) f = (rev ? self.theme->bg : self.theme->fg);
    if (self.boldAsBright && (c->flags & VT_BOLD) && VT_COLOR_IS_INDEXED(f) && VT_COLOR_INDEX(f) < 8)
        f = VT_COLOR_IDX(VT_COLOR_INDEX(f) + 8);
    if (c->flags & VT_INVISIBLE) f = (b == VT_COLOR_DEFAULT ? self.theme->bg : b);
    return f;
}

- (uint32_t)effectiveBgForCell:(const VtCell *)c
{
    uint32_t f = c->fg, b = c->bg;
    BOOL rev = (self.vt && self.vt->reverse_video) ^ ((c->flags & VT_REVERSE) != 0);
    if (rev) { uint32_t t = f; f = b; b = t; }
    if (b == VT_COLOR_DEFAULT) return VT_COLOR_DEFAULT;
    return b;
}

- (CTFontRef)fontForCell:(const VtCell *)c
{
    BOOL bold = (c->flags & VT_BOLD) != 0;
    BOOL italic = (c->flags & VT_ITALIC) != 0;
    if (bold && italic) return _boldItalicFont;
    if (bold) return _boldFont;
    if (italic) return _italicFont;
    return _font;
}

/* ================= 滚动 ================= */

- (NSInteger)maxScroll
{
    if (!self.vt) return 0;
    return (NSInteger)vt_sb_offset(self.vt);
}

- (NSInteger)scrollOffsetValue { return _scrollOffset; }

- (VtLine *)lineForDisplayRow:(int)y
{
    if (!self.vt) return NULL;
    NSInteger max = [self maxScroll];
    if (_scrollOffset > max) _scrollOffset = max;
    if (_scrollOffset < 0) _scrollOffset = 0;
    NSInteger base = (NSInteger)vt_sb_offset(self.vt) - _scrollOffset;
    return vt_line_at(self.vt, (int)(base + y));
}

- (void)scrollByLines:(NSInteger)lines
{
    if (!self.vt) return;
    if (self.vt->alt_active) return;
    NSInteger old = _scrollOffset;
    _scrollOffset += lines;
    NSInteger max = [self maxScroll];
    if (_scrollOffset > max) _scrollOffset = max;
    if (_scrollOffset < 0) _scrollOffset = 0;
    if (old != _scrollOffset) {
        [self setNeedsDisplay];
        [self.delegate terminalViewDidScroll:self];
    }
}

- (void)scrollToBottom
{
    if (_scrollOffset != 0) {
        _scrollOffset = 0;
        [self setNeedsDisplay];
        [self.delegate terminalViewDidScroll:self];
    }
}

- (void)resetBlink
{
    _cursorOn = YES;
    [_blinkTimer invalidate];
    _blinkTimer = nil;
    if (self.cursorBlink && self.window) {
        __weak TerminalView *weakSelf = self;
        _blinkTimer = [NSTimer scheduledTimerWithTimeInterval:0.55 repeats:YES block:^(NSTimer *t) {
            TerminalView *v = weakSelf;
            if (!v) { [t invalidate]; return; }
            v->_cursorOn = !v->_cursorOn;
            [v setNeedsDisplay];
        }];
    }
}

- (void)didMoveToWindow
{
    [super didMoveToWindow];
    if (self.window) [self resetBlink];
    else {
        [_blinkTimer invalidate];
        _blinkTimer = nil;
    }
}

/* ================= 绘制 ================= */

- (void)terminalDidUpdate
{
    if (!self.vt) return;
    if (self.vt->alt_active && _scrollOffset != 0) {
        _scrollOffset = 0;
    }
    NSInteger cx = vt_cursor_x(self.vt), cy = vt_cursor_y(self.vt);
    if (cx != _lastCursorX || cy != _lastCursorY) {
        _lastCursorX = cx;
        _lastCursorY = cy;
        _cursorOn = YES;
        if (self.window) {
            [_inputDelegate selectionWillChange:self];
            [_inputDelegate selectionDidChange:self];
        }
    }
    [self refreshPaletteIfNeeded];

    int y0 = 0, y1 = 0;
    BOOL dirty = vt_take_dirty(self.vt, &y0, &y1);
    if (dirty) {
        [self setNeedsDisplay];
    }
}

- (void)drawRect:(CGRect)rect
{
    CGContextRef ctx = UIGraphicsGetCurrentContext();
    [[self backgroundColor] setFill];
    CGContextFillRect(ctx, rect);
    if (!self.vt || _cellW <= 0) return;
    [self ensureCache];
    CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
    CGContextSetShouldSmoothFonts(ctx, NO);

    int y0 = (int)floor(rect.origin.y / _cellH);
    int y1 = (int)ceil(CGRectGetMaxY(rect) / _cellH);
    if (y0 < 0) y0 = 0;
    if (y1 > _rows - 1) y1 = _rows - 1;
    for (int y = y0; y <= y1; y++) [self drawRow:y ctx:ctx];
    [self drawSelection:ctx];
    [self drawMarkedText:ctx];
    [self drawCursor:ctx];
    [self drawScrollIndicator:ctx];
}

- (void)drawRow:(int)y ctx:(CGContextRef)ctx
{
    VtLine *l = [self lineForDisplayRow:y];
    if (!l || !l->cells) return;
    CGFloat oy = y * _cellH;
    /* 背景 */
    int runX0 = -1;
    uint32_t runBg = VT_COLOR_DEFAULT;
    for (int x = 0; x < l->cols; x++) {
        uint32_t bg = [self effectiveBgForCell:&l->cells[x]];
        BOOL isDefault = (bg == VT_COLOR_DEFAULT);
        if (!isDefault && runX0 < 0) {
            runX0 = x;
            runBg = bg;
        } else if (runX0 >= 0 && (isDefault || bg != runBg)) {
            [[self colorForVT:runBg fallback:self.theme->bg] setFill];
            CGContextFillRect(ctx, CGRectMake(runX0 * _cellW, oy, (x - runX0) * _cellW, _cellH));
            runX0 = isDefault ? -1 : x;
            runBg = bg;
        }
    }
    if (runX0 >= 0) {
        [[self colorForVT:runBg fallback:self.theme->bg] setFill];
        CGContextFillRect(ctx, CGRectMake(runX0 * _cellW, oy, (l->cols - runX0) * _cellW, _cellH));
    }
    /* 文字 */
    CTLineRef line = [self lineForRow:y vtLine:l];
    if (line) {
        CGContextSetTextPosition(ctx, 0, oy + _ascent);
        CTLineDraw(line, ctx);
    }
}

- (CTLineRef)lineForRow:(int)y vtLine:(VtLine *)l
{
    if (y >= 0 && y < _cacheCount) {
        TermRowCache *e = &_cache[y];
        if (e->line && e->linePtr == (const void *)l && e->rev == l->rev && e->cols == l->cols)
            return e->line;
    }
    CTLineRef built = [self buildLine:l];
    if (y >= 0 && y < _cacheCount) {
        TermRowCache *e = &_cache[y];
        if (e->line) CFRelease(e->line);
        e->line = built;
        e->linePtr = l;
        e->rev = l->rev;
        e->cols = l->cols;
    } else if (built) {
        CFRelease(built);
    }
    return built;
}

- (CTLineRef)buildLine:(VtLine *)l
{
    if (!l || l->cols == 0) return NULL;
    NSMutableAttributedString *as = [[NSMutableAttributedString alloc] init];
    TermSpan *spans = (TermSpan *)calloc((size_t)l->cols + 2, sizeof(TermSpan));
    int nspans = 0;
    NSUInteger loc = 0;
    for (int x = 0; x < l->cols; x++) {
        VtCell *c = &l->cells[x];
        if (c->width == 0) continue;     /* 宽字符后半格 */
        NSString *s = TermUniCharToString(c->cp);
        if (c->cp2) s = [s stringByAppendingString:TermUniCharToString(c->cp2)];
        if (c->cp3) s = [s stringByAppendingString:TermUniCharToString(c->cp3)];
        NSUInteger len = s.length;
        [as appendAttributedString:[[NSAttributedString alloc] initWithString:s]];
        CTFontRef font = [self fontForCell:c];
        uint32_t fg = [self effectiveFgForCell:c];
        uint32_t bg = [self effectiveBgForCell:c];
        int cells = c->width ? c->width : 1;
        int cluster = (c->cp2 || c->cp3 || c->cp > 0xFFFF) ? 1 : 0;
        NSMutableDictionary *attrs = [NSMutableDictionary dictionary];
        attrs[(__bridge NSString *)kCTFontAttributeName] = (__bridge id)font;
        attrs[(__bridge NSString *)kCTForegroundColorAttributeName] =
            (__bridge id)[self colorForVT:fg fallback:self.theme->fg].CGColor;
        if (c->flags & VT_UNDERLINE)
            attrs[(__bridge NSString *)kCTUnderlineStyleAttributeName] = @(kCTUnderlineStyleSingle);
        else if (c->flags & VT_DBLUNDER)
            attrs[(__bridge NSString *)kCTUnderlineStyleAttributeName] = @(kCTUnderlineStyleDouble);
        if (c->flags & VT_STRIKE)
            attrs[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
        [as addAttributes:attrs range:NSMakeRange(loc, len)];

        if (nspans > 0 && !cluster && !spans[nspans - 1].cluster &&
            spans[nspans - 1].font == font && spans[nspans - 1].fg == fg &&
            spans[nspans - 1].flags == c->flags) {
            spans[nspans - 1].len += len;
            spans[nspans - 1].cells += cells;
        } else {
            spans[nspans].loc = loc;
            spans[nspans].len = len;
            spans[nspans].cells = cells;
            spans[nspans].font = font;
            spans[nspans].fg = fg;
            spans[nspans].bg = bg;
            spans[nspans].flags = c->flags;
            spans[nspans].cluster = cluster;
            nspans++;
        }
        loc += len;
    }
    /* 字距对齐：保证每格正好 _cellW 宽 */
    for (int i = 0; i < nspans; i++) {
        NSAttributedString *sub = [as attributedSubstringFromRange:NSMakeRange(spans[i].loc, spans[i].len)];
        CGFloat adv = [self advanceOfAttributed:sub];
        CGFloat want = spans[i].cells * _cellW;
        CGFloat kern = want - adv;
        if (spans[i].cluster || spans[i].len == 0) {
            [as addAttribute:(__bridge NSString *)kCTKernAttributeName
                       value:@(kern)
                       range:NSMakeRange(spans[i].loc + spans[i].len - 1, 1)];
        } else if (spans[i].len > 0) {
            [as addAttribute:(__bridge NSString *)kCTKernAttributeName
                       value:@(kern / (CGFloat)spans[i].cells)
                       range:NSMakeRange(spans[i].loc, spans[i].len)];
        }
    }
    free(spans);
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as);
    return line;
}

/* ================= 选区 / 光标 / 滚动条 ================= */

- (void)drawSelection:(CGContextRef)ctx
{
    if (!_hasSelection) return;
    NSInteger r0 = _selAnchorRow, c0 = _selAnchorCol, r1 = _selHeadRow, c1 = _selHeadCol;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        NSInteger t;
        t = r0; r0 = r1; r1 = t;
        t = c0; c0 = c1; c1 = t;
    }
    NSInteger base = (NSInteger)vt_sb_offset(self.vt) - _scrollOffset;
    [[self colorForVT:self.theme->selection fallback:0x334455] setFill];
    for (NSInteger r = r0; r <= r1; r++) {
        NSInteger dy = r - base;
        if (dy < 0 || dy >= _rows) continue;
        VtLine *l = [self lineForDisplayRow:(int)dy];
        if (!l) continue;
        NSInteger x0 = (r == r0) ? c0 : 0;
        NSInteger x1 = (r == r1) ? c1 + 1 : l->cols;
        if (x0 < 0) x0 = 0;
        if (x1 > l->cols) x1 = l->cols;
        if (x1 <= x0) continue;
        CGContextFillRect(ctx, CGRectMake(x0 * _cellW, dy * _cellH, (x1 - x0) * _cellW, _cellH));
    }
}

- (void)drawCursor:(CGContextRef)ctx
{
    if (!self.vt || !vt_cursor_visible(self.vt)) return;
    if (_scrollOffset != 0) return;
    if (!_cursorOn) return;
    if (_markedText.length) return;   /* 输入法状态画预编辑文本 */
    int cx = vt_cursor_x(self.vt), cy = vt_cursor_y(self.vt);
    VtLine *l = [self lineForDisplayRow:cy];
    if (!l || cx >= l->cols) return;
    int w = l->cells[cx].width == 2 ? 2 : 1;
    CGRect r = CGRectMake(cx * _cellW, cy * _cellH, w * _cellW, _cellH);
    int style = self.vt->cursor_style;
    if (style == 0) style = 1;
    CGContextSetFillColorWithColor(ctx, [self colorForVT:self.theme->cursor fallback:0xFFFFFF].CGColor);
    if (style == 5 || style == 6) {
        CGContextFillRect(ctx, CGRectMake(r.origin.x, r.origin.y, MAX(2.0, _cellW * 0.15), r.size.height));
    } else if (style == 3 || style == 4) {
        CGContextFillRect(ctx, CGRectMake(r.origin.x, CGRectGetMaxY(r) - MAX(2.0, _cellH * 0.12), r.size.width, MAX(2.0, _cellH * 0.12)));
    } else {
        CGContextFillRect(ctx, r);
        /* 把光标下的字反色画出来 */
        VtCell *c = &l->cells[cx];
        if (c->cp && c->cp != ' ') {
            NSString *s = TermUniCharToString(c->cp);
            if (c->cp2) s = [s stringByAppendingString:TermUniCharToString(c->cp2)];
            NSDictionary *a = @{
                (__bridge NSString *)kCTFontAttributeName: (__bridge id)[self fontForCell:c],
                (__bridge NSString *)kCTForegroundColorAttributeName: (__bridge id)[self backgroundColor].CGColor
            };
            NSAttributedString *as = [[NSAttributedString alloc] initWithString:s attributes:a];
            CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as);
            CGContextSetTextPosition(ctx, r.origin.x, r.origin.y + _ascent);
            CTLineDraw(line, ctx);
            CFRelease(line);
        }
    }
}

- (void)drawScrollIndicator:(CGContextRef)ctx
{
    NSInteger max = [self maxScroll];
    if (max <= 0) return;
    if (_scrollOffset == 0) return;
    CGFloat h = self.bounds.size.height;
    CGFloat barH = MAX(28.0, h * (CGFloat)_rows / (CGFloat)(max + _rows));
    CGFloat t = (CGFloat)(max - _scrollOffset) / (CGFloat)max;
    CGFloat y = t * (h - barH);
    CGContextSetFillColorWithColor(ctx, [[UIColor colorWithWhite:1.0 alpha:0.22] CGColor]);
    CGContextFillRect(ctx, CGRectMake(self.bounds.size.width - 4, y, 3, barH));
}

/* ================= 坐标换算 / 命中测试 ================= */

#define TERM_ROW_STRIDE 100000

- (NSInteger)totalRows
{
    return self.vt ? (NSInteger)vt_total_lines(self.vt) : 0;
}

- (NSInteger)baseGlobalRow
{
    if (!self.vt) return 0;
    return (NSInteger)vt_sb_offset(self.vt) - _scrollOffset;
}

- (VtLine *)lineAtGlobalRow:(NSInteger)r
{
    if (!self.vt || r < 0 || r >= (NSInteger)vt_total_lines(self.vt)) return NULL;
    return vt_line_at(self.vt, (int)r);
}

/* 某一格的真字符：宽字符的后半格要往左借一格 */
- (uint32_t)cpAtRow:(NSInteger)r col:(NSInteger)c
{
    VtLine *l = [self lineAtGlobalRow:r];
    if (!l || c < 0 || c >= l->cols) return 0;
    VtCell *cell = &l->cells[c];
    if (cell->width == 0 && c > 0) cell = &l->cells[c - 1];
    return cell->cp;
}

/* 屏幕点 -> 缓冲坐标；col 允许等于 cols(拖到行尾) */
- (BOOL)cellAtPoint:(CGPoint)p row:(NSInteger *)outRow col:(NSInteger *)outCol
{
    if (!self.vt || _rows < 1 || _cellW <= 0 || _cellH <= 0) return NO;
    NSInteger dy = (NSInteger)floor(p.y / _cellH);
    if (dy < 0) dy = 0;
    if (dy > _rows - 1) dy = _rows - 1;
    NSInteger r = [self baseGlobalRow] + dy;
    NSInteger dx = (NSInteger)floor(p.x / _cellW);
    if (dx < 0) dx = 0;
    if (dx > _cols) dx = _cols;
    VtLine *l = [self lineAtGlobalRow:r];
    if (l && dx > l->cols) dx = l->cols;
    if (outRow) *outRow = r;
    if (outCol) *outCol = dx;
    return YES;
}

/* 显示行 y 上、第 x 列那一格的矩形 */
- (CGRect)rectForDisplayRow:(NSInteger)y col:(NSInteger)x width:(NSInteger)w
{
    return CGRectMake(x * _cellW, y * _cellH, MAX(1, w) * _cellW, _cellH);
}

- (CGRect)rectForGlobalRow:(NSInteger)r col:(NSInteger)c width:(NSInteger)w
{
    NSInteger dy = r - [self baseGlobalRow];
    return CGRectMake(c * _cellW, dy * _cellH, MAX(1, w) * _cellW, _cellH);
}

static BOOL TermIsWordCp(uint32_t c)
{
    if (c == 0 || c == ' ' || c == '\t') return NO;
    if (c < 0x80) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return YES;
        return c == '_' || c == '-' || c == '.' || c == '/' || c == '~' ||
               c == ':' || c == '\\' || c == '@' || c == '=';
    }
    if (c >= 0x00A0 && c <= 0x00BF) return NO;   /* 拉丁标点 */
    if (c >= 0x2000 && c <= 0x206F) return NO;   /* 通用标点 */
    if (c >= 0x3000 && c <= 0x303F) return NO;   /* CJK 标点 */
    if (c >= 0xFF01 && c <= 0xFF20) return NO;   /* 全角标点/数字前段 */
    if (c >= 0xFF3B && c <= 0xFF40) return NO;
    if (c >= 0xFF5B && c <= 0xFF65) return NO;
    return YES;                                   /* 汉字/假名/emoji 都算词字符 */
}

- (void)wordRangeAtRow:(NSInteger)r col:(NSInteger)c start:(NSInteger *)sp end:(NSInteger *)ep
{
    VtLine *l = [self lineAtGlobalRow:r];
    NSInteger s = 0, e = 0;
    if (l && l->cols > 0) {
        NSInteger x = c;
        if (x >= l->cols) x = l->cols - 1;
        if (x < 0) x = 0;
        if (TermIsWordCp([self cpAtRow:r col:x])) {
            s = x; e = x;
            while (s > 0 && TermIsWordCp([self cpAtRow:r col:s - 1])) s--;
            while (e + 1 < l->cols && TermIsWordCp([self cpAtRow:r col:e + 1])) e++;
            e++;
        } else {
            s = x; e = x;
        }
    }
    if (sp) *sp = s;
    if (ep) *ep = e;
}

/* ================= 选区 ================= */

- (BOOL)hasSelection { return _hasSelection; }

- (void)orderedSelRow0:(NSInteger *)r0 col0:(NSInteger *)c0 row1:(NSInteger *)r1 col1:(NSInteger *)c1
{
    NSInteger ar = _selAnchorRow, ac = _selAnchorCol, hr = _selHeadRow, hc = _selHeadCol;
    if (ar > hr || (ar == hr && ac > hc)) {
        NSInteger t;
        t = ar; ar = hr; hr = t;
        t = ac; ac = hc; hc = t;
    }
    if (r0) *r0 = ar;
    if (c0) *c0 = ac;
    if (r1) *r1 = hr;
    if (c1) *c1 = hc;
}

- (void)setSelectionFromRow:(NSInteger)r0 col:(NSInteger)c0 toRow:(NSInteger)r1 col:(NSInteger)c1
{
    _hasSelection = YES;
    _selAnchorRow = r0;
    _selAnchorCol = c0;
    _selHeadRow = r1;
    _selHeadCol = c1;
    [self setNeedsDisplay];
}

- (void)clearSelection
{
    if (!_hasSelection) return;
    _hasSelection = NO;
    [self setNeedsDisplay];
    [[UIMenuController sharedMenuController] setMenuVisible:NO animated:NO];
}

- (void)selectAll
{
    NSInteger total = [self totalRows];
    if (total < 1) return;
    NSInteger lastRow = total - 1;
    VtLine *l = [self lineAtGlobalRow:lastRow];
    [self setSelectionFromRow:0 col:0 toRow:lastRow col:(l ? l->cols : 0)];
}

- (NSString *)selectedText
{
    if (!_hasSelection || !self.vt) return @"";
    NSInteger r0, c0, r1, c1;
    [self orderedSelRow0:&r0 col0:&c0 row1:&r1 col1:&c1];
    char *t = vt_range_text(self.vt, (int)r0, (int)c0, (int)r1, (int)c1);
    NSString *s = t ? [NSString stringWithUTF8String:t] : nil;
    free(t);
    return s ?: @"";
}

- (void)copySelection
{
    NSString *s = [self selectedText];
    if (!s.length) return;
    [UIPasteboard generalPasteboard].string = s;
    [self clearSelection];
}

- (BOOL)shouldConfirmPaste:(NSString *)s
{
    if (!self.confirmMultilinePaste) return NO;
    return [s rangeOfString:@"\n"].location != NSNotFound ||
           [s rangeOfString:@"\r"].location != NSNotFound;
}

- (void)pasteFromPasteboard
{
    NSString *s = [UIPasteboard generalPasteboard].string;
    if (!s.length) return;
    if ([self shouldConfirmPaste:s] &&
        [self.delegate respondsToSelector:@selector(terminalView:confirmPaste:bracketed:)]) {
        [self.delegate terminalView:self confirmPaste:s bracketed:self.bracketPaste];
        return;
    }
    [self pasteString:s withBrackets:self.bracketPaste];
}

- (void)pasteString:(NSString *)text withBrackets:(BOOL)brackets
{
    if (!text.length || !self.vt) return;
    [self clearSelection];
    NSMutableString *s = [NSMutableString stringWithString:text];
    [s replaceOccurrencesOfString:@"\r\n" withString:@"\n" options:0 range:NSMakeRange(0, s.length)];
    [s replaceOccurrencesOfString:@"\r" withString:@"\n" options:0 range:NSMakeRange(0, s.length)];
    if (brackets && self.vt->bracketed_paste) {
        [self sendString:@"\x1b[200~"];
        [self sendString:s];
        [self sendString:@"\x1b[201~"];
    } else {
        if (brackets) {
            /* 远端没开括号粘贴，换行得自己转成回车，否则 shell 只看到一坨 */
            [s replaceOccurrencesOfString:@"\n" withString:@"\r" options:0 range:NSMakeRange(0, s.length)];
        }
        [self sendString:s];
    }
    [self scrollToBottom];
    [self resetBlink];
}

- (void)showEditMenu
{
    if (!self.window) return;
    [self becomeFirstResponder];
    CGRect r;
    if (_hasSelection) {
        NSInteger r0, c0, r1, c1;
        [self orderedSelRow0:&r0 col0:&c0 row1:&r1 col1:&c1];
        NSInteger dy = r1 - [self baseGlobalRow];
        if (dy < 0) dy = 0;
        if (dy > _rows - 1) dy = _rows - 1;
        r = [self rectForDisplayRow:dy col:c0 width:1];
        if ([self lineAtGlobalRow:r1] == NULL) return;
    } else {
        int cx = vt_cursor_x(self.vt), cy = vt_cursor_y(self.vt);
        if (_scrollOffset != 0) { cy = _rows - 1; cx = 0; }
        r = [self rectForDisplayRow:cy col:cx width:2];
    }
    if (CGRectIsEmpty(r)) return;
    UIMenuController *menu = [UIMenuController sharedMenuController];
    if (menu.isMenuVisible) [menu setMenuVisible:NO animated:NO];
    [menu setTargetRect:CGRectInset(r, 0, -2) inView:self];
    [menu setMenuVisible:YES animated:YES];
}

- (BOOL)canBecomeFirstResponder { return YES; }
- (BOOL)canResignFirstResponder { return YES; }

- (UIView *)inputAccessoryView { return self.keyBar; }

- (BOOL)canPerformAction:(SEL)action withSender:(id)sender
{
    if (action == @selector(copy:) || action == @selector(cut:)) return _hasSelection;
    if (action == @selector(paste:)) return [UIPasteboard generalPasteboard].string.length > 0;
    if (action == @selector(selectAll:)) return [self totalRows] > 0;
    return NO;
}

- (void)copy:(id)sender { [self copySelection]; }
- (void)cut:(id)sender { [self copySelection]; }
- (void)paste:(id)sender { [self pasteFromPasteboard]; }
- (void)selectAll:(id)sender { [self selectAll]; }

/* ================= 手势 ================= */

- (BOOL)gestureRecognizer:(UIGestureRecognizer *)g shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)other
{
    return YES;   /* 捏合/平移/长按可以共存，靠 _selectionDrag 判断谁说了算 */
}

- (void)setScrollOffsetValue:(NSInteger)v
{
    NSInteger max = [self maxScroll];
    if (v > max) v = max;
    if (v < 0) v = 0;
    if (v == _scrollOffset) return;
    _scrollOffset = v;
    [self setNeedsDisplay];
    [self.delegate terminalViewDidScroll:self];
}

- (void)scrollToTop
{
    [self setScrollOffsetValue:[self maxScroll]];
}

- (void)handlePan:(UIPanGestureRecognizer *)g
{
    if (_selectionDrag) return;
    if (g.state == UIGestureRecognizerStateBegan) {
        _panStartOffset = _scrollOffset;
        CGPoint v = [g velocityInView:self];
        if (fabs(v.y) > 400) [[UIMenuController sharedMenuController] setMenuVisible:NO animated:NO];
    } else if (g.state == UIGestureRecognizerStateChanged) {
        CGFloat dy = [g translationInView:self].y;
        NSInteger lines = (NSInteger)floor(dy / MAX(1.0, _cellH));
        [self setScrollOffsetValue:_panStartOffset + lines];
    }
}

- (void)handlePinch:(UIPinchGestureRecognizer *)g
{
    if (g.state == UIGestureRecognizerStateBegan) {
        _pinchBaseSize = self.fontSize;
    } else if (g.state == UIGestureRecognizerStateChanged) {
        CGFloat s = _pinchBaseSize * g.scale;
        s = round(s);
        if (s < 8) s = 8;
        if (s > 32) s = 32;
        if (fabs(s - self.fontSize) >= 0.5) self.fontSize = s;
    } else if (g.state == UIGestureRecognizerStateEnded || g.state == UIGestureRecognizerStateCancelled) {
        if ([self.delegate respondsToSelector:@selector(terminalView:didChangeFontSize:)])
            [self.delegate terminalView:self didChangeFontSize:self.fontSize];
    }
}

- (void)sendMouseEvent:(int)btn row:(NSInteger)r col:(NSInteger)c press:(BOOL)press
{
    if (!self.vt || !self.vt->mouse_mode) return;
    NSInteger y = r - (NSInteger)vt_sb_offset(self.vt) + 1;
    NSInteger x = c + 1;
    char buf[64];
    int n;
    if (self.vt->mouse_sgr) {
        n = snprintf(buf, sizeof(buf), "\x1b[<%d;%ld;%ld%c", btn, (long)x, (long)y, press ? 'M' : 'm');
    } else {
        if (!press) return;   /* 老式编码只有按下 */
        if (x > 223) x = 223;
        if (y > 223) y = 223;
        n = snprintf(buf, sizeof(buf), "\x1b[M%c%c%c", (char)(32 + btn), (char)(32 + x), (char)(32 + y));
    }
    if (n > 0) [self.delegate terminalView:self sendBytes:buf length:(NSUInteger)n];
}

- (void)handleTap:(UITapGestureRecognizer *)g
{
    if (!self.window) return;
    if (_hasSelection) {
        [self clearSelection];
        return;
    }
    if (![self isFirstResponder]) {
        [self becomeFirstResponder];
        return;
    }
    if (self.sendMouseEvents && self.vt && self.vt->mouse_mode) {
        NSInteger r, c;
        if (![self cellAtPoint:[g locationInView:self] row:&r col:&c]) return;
        [self sendMouseEvent:0 row:r col:c press:YES];
        [self sendMouseEvent:0 row:r col:c press:NO];
        return;
    }
    if ([[UIMenuController sharedMenuController] isMenuVisible])
        [[UIMenuController sharedMenuController] setMenuVisible:NO animated:YES];
}

- (void)handleDoubleTap:(UITapGestureRecognizer *)g
{
    NSInteger r, c;
    if (![self cellAtPoint:[g locationInView:self] row:&r col:&c]) return;
    NSInteger s = 0, e = 0;
    [self wordRangeAtRow:r col:c start:&s end:&e];
    if (e <= s) {
        VtLine *l = [self lineAtGlobalRow:r];
        s = 0;
        e = l ? l->cols : 0;
    }
    [self setSelectionFromRow:r col:s toRow:r col:MAX(s, e - 1)];
    [self showEditMenu];
}

- (void)handleTripleTap:(UITapGestureRecognizer *)g
{
    NSInteger r, c;
    if (![self cellAtPoint:[g locationInView:self] row:&r col:&c]) return;
    VtLine *l = [self lineAtGlobalRow:r];
    NSInteger cols = l ? l->cols : 0;
    if (cols > 0) cols--;
    [self setSelectionFromRow:r col:0 toRow:r col:cols];
    [self showEditMenu];
}

- (void)handleLongPress:(UILongPressGestureRecognizer *)g
{
    CGPoint p = [g locationInView:self];
    NSInteger r, c;
    if (g.state == UIGestureRecognizerStateBegan) {
        if (![self cellAtPoint:p row:&r col:&c]) return;
        _selectionDrag = YES;
        NSInteger s = 0, e = 0;
        [self wordRangeAtRow:r col:c start:&s end:&e];
        [self setSelectionFromRow:r col:s toRow:r col:MAX(s, e - 1)];
        if (e <= s) {
            /* 空白处：不要选区，直接给粘贴菜单 */
            _hasSelection = NO;
            [self setNeedsDisplay];
        }
    } else if (g.state == UIGestureRecognizerStateChanged) {
        if (!_selectionDrag) return;
        if (![self cellAtPoint:p row:&r col:&c]) return;
        if (!_hasSelection) {
            _hasSelection = YES;
            _selAnchorRow = r;
            _selAnchorCol = c;
        }
        _selHeadRow = r;
        _selHeadCol = c;
        [self setNeedsDisplay];
    } else if (g.state == UIGestureRecognizerStateEnded || g.state == UIGestureRecognizerStateCancelled) {
        _selectionDrag = NO;
        [self showEditMenu];
    }
}

/* ================= 折叠/预编辑文本 ================= */

- (NSUInteger)cellWidthOfString:(NSString *)s
{
    NSUInteger cells = 0;
    for (NSUInteger i = 0; i < s.length; i++) {
        uint32_t cp = [s characterAtIndex:i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.length) {
            uint32_t lo = [s characterAtIndex:i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        int w = vt_cell_width(cp);
        if (w > 0) cells += (NSUInteger)w;
    }
    return cells;
}

/* 从尾巴上截出能塞进 avail 个格子的部分（输入法串太长时保证光标可见） */
- (NSString *)tailOfString:(NSString *)s fittingCells:(NSInteger)avail
{
    if (avail <= 0) return @"";
    NSInteger total = (NSInteger)[self cellWidthOfString:s];
    if (total <= avail) return s;
    NSInteger drop = total - avail;
    NSUInteger i = 0;
    NSInteger dropped = 0;
    while (i < s.length && dropped < drop) {
        uint32_t cp = [s characterAtIndex:i];
        NSUInteger step = 1;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.length) {
            uint32_t lo = [s characterAtIndex:i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                step = 2;
            }
        }
        int w = vt_cell_width(cp);
        if (w > 0) dropped += w;
        i += step;
    }
    return [s substringFromIndex:i];
}

- (BOOL)hasMarkedText { return _markedText.length > 0; }

- (void)drawMarkedText:(CGContextRef)ctx
{
    if (!_markedText.length || !self.vt) return;
    if (_scrollOffset != 0) return;
    int cx = vt_cursor_x(self.vt), cy = vt_cursor_y(self.vt);
    if (cx >= _cols) cx = _cols - 1;
    if (cy >= _rows) cy = _rows - 1;
    if (cx < 0 || cy < 0) return;
    NSString *vis = [self tailOfString:_markedText fittingCells:_cols - cx - 1];
    if (!vis.length) return;
    CGFloat x = cx * _cellW;
    CGFloat y = cy * _cellH;
    NSUInteger cells = [self cellWidthOfString:vis];
    [[self colorForVT:0xFFFFFFFFu fallback:self.theme->bg] setFill];
    CGContextFillRect(ctx, CGRectMake(x, y, (CGFloat)cells * _cellW + 2, _cellH));

    NSDictionary *attrs = @{
        (__bridge NSString *)kCTFontAttributeName: (__bridge id)_font,
        (__bridge NSString *)kCTForegroundColorAttributeName:
            (__bridge id)[self colorForVT:0xFFFFFFFFu fallback:self.theme->fg].CGColor,
        (__bridge NSString *)kCTUnderlineStyleAttributeName: @(kCTUnderlineStyleSingle),
    };
    NSAttributedString *as = [[NSAttributedString alloc] initWithString:vis attributes:attrs];
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as);
    double w = 0;
    CTLineGetTypographicBounds(line, NULL, NULL, &w);
    CGContextSetTextPosition(ctx, x, y + _ascent);
    CTLineDraw(line, ctx);
    CFRelease(line);
    /* 光标画在输入串后面 */
    CGContextSetFillColorWithColor(ctx, [self colorForVT:self.theme->cursor fallback:0xFFFFFF].CGColor);
    CGContextFillRect(ctx, CGRectMake(x + (CGFloat)w + 1, y, MAX(2.0, _cellW * 0.12), _cellH));
}

/* ================= 发键 ================= */

- (void)consumeStickyModifiers
{
    if (!_stickyCtrl && !_stickyAlt) return;
    _stickyCtrl = NO;
    _stickyAlt = NO;
    if ([self.delegate respondsToSelector:@selector(terminalViewDidChangeStickyModifiers:)])
        [self.delegate terminalViewDidChangeStickyModifiers:self];
}

- (void)sendKey:(int)key mods:(int)mods ch:(uint32_t)ch
{
    if (!self.vt) return;
    int m = mods;
    if (key == VK_CHAR) {
        if (_stickyCtrl) m |= VTM_CTRL;
        if (_stickyAlt) m |= VTM_ALT;
    }
    if (_hasSelection && key != VK_CHAR) [self clearSelection];
    char buf[64];
    int n = vt_key_encode(self.vt, key, m, ch, buf, sizeof(buf));
    if (n > 0) [self.delegate terminalView:self sendBytes:buf length:(NSUInteger)n];
    switch (key) {
    case VK_CHAR: case VK_ENTER: case VK_TAB: case VK_BACKSPACE:
        [self scrollToBottom];
        break;
    default:
        break;
    }
    [self resetBlink];
    [self consumeStickyModifiers];
}

- (void)sendString:(NSString *)str
{
    if (!str.length || !self.vt) return;
    const char *utf8 = [str UTF8String];
    if (!utf8) return;
    size_t n = strlen(utf8);
    if (n) [self.delegate terminalView:self sendBytes:utf8 length:n];
}

/* ================= UITextInputTraits ================= */

@synthesize keyboardType = _keyboardType;
@synthesize autocapitalizationType = _autocapitalizationType;
@synthesize autocorrectionType = _autocorrectionType;
@synthesize spellCheckingType = _spellCheckingType;
@synthesize enablesReturnKeyAutomatically = _enablesReturnKeyAutomatically;
@synthesize keyboardAppearance = _keyboardAppearance;
@synthesize returnKeyType = _returnKeyType;
@synthesize secureTextEntry = _secureTextEntry;

/* ================= UITextInput ================= */

- (UIView *)textInputView { return self; }

- (UITextPosition *)beginningOfDocument { return [TermPosition pos:0]; }

- (UITextPosition *)endOfDocument
{
    NSInteger total = [self totalRows];
    if (total < 1) return [TermPosition pos:0];
    VtLine *l = [self lineAtGlobalRow:total - 1];
    return [TermPosition pos:(total - 1) * TERM_ROW_STRIDE + (l ? l->cols : 0)];
}

- (NSInteger)caretOffset
{
    if (!self.vt) return 0;
    NSInteger row = [self baseGlobalRow] + vt_cursor_y(self.vt);
    return row * TERM_ROW_STRIDE + vt_cursor_x(self.vt);
}

- (void)offsetToRow:(int *)outRow col:(int *)outCol offset:(NSInteger)o
{
    if (outRow) *outRow = (int)(o / TERM_ROW_STRIDE);
    if (outCol) *outCol = (int)(o % TERM_ROW_STRIDE);
}

- (TermRange *)rangeFrom:(NSInteger)s to:(NSInteger)e
{
    TermRange *r = [[TermRange alloc] init];
    r.startPos = [TermPosition pos:s];
    r.endPos = [TermPosition pos:e];
    return r;
}

- (UITextRange *)textRangeFromPosition:(UITextPosition *)from toPosition:(UITextPosition *)to
{
    if (![from isKindOfClass:[TermPosition class]] || ![to isKindOfClass:[TermPosition class]])
        return nil;
    return [self rangeFrom:((TermPosition *)from).offset to:((TermPosition *)to).offset];
}

- (UITextPosition *)positionFromPosition:(UITextPosition *)pos offset:(NSInteger)delta
{
    if (![pos isKindOfClass:[TermPosition class]]) return nil;
    NSInteger o = ((TermPosition *)pos).offset + delta;
    if (o < 0) o = 0;
    return [TermPosition pos:o];
}

- (UITextPosition *)positionFromPosition:(UITextPosition *)pos
                             inDirection:(UITextLayoutDirection)dir
                                  offset:(NSInteger)delta
{
    if (![pos isKindOfClass:[TermPosition class]]) return nil;
    NSInteger o = ((TermPosition *)pos).offset;
    switch (dir) {
    case UITextLayoutDirectionLeft:
    case UITextLayoutDirectionRight:
        o += (dir == UITextLayoutDirectionLeft ? -delta : delta);
        break;
    default:
        o += delta * TERM_ROW_STRIDE;
        break;
    }
    if (o < 0) o = 0;
    return [TermPosition pos:o];
}

- (UITextRange *)characterRangeByExtendingPosition:(UITextPosition *)pos
                                       inDirection:(UITextLayoutDirection)dir
{
    if (![pos isKindOfClass:[TermPosition class]]) return nil;
    NSInteger o = ((TermPosition *)pos).offset;
    if (dir == UITextLayoutDirectionLeft || dir == UITextLayoutDirectionUp)
        return [self rangeFrom:MAX((NSInteger)0, o - 1) to:o];
    return [self rangeFrom:o to:o + 1];
}

- (UITextRange *)characterRangeAtPoint:(CGPoint)point
{
    NSInteger r, c;
    if (![self cellAtPoint:point row:&r col:&c]) return nil;
    NSInteger o = r * TERM_ROW_STRIDE + c;
    return [self rangeFrom:o to:o + 1];
}

- (UITextPosition *)closestPositionToPoint:(CGPoint)point
{
    NSInteger r, c;
    if (![self cellAtPoint:point row:&r col:&c]) return nil;
    return [TermPosition pos:r * TERM_ROW_STRIDE + c];
}

- (UITextPosition *)closestPositionToPoint:(CGPoint)point withinRange:(UITextRange *)range
{
    return [self closestPositionToPoint:point];
}

- (NSString *)textInRange:(UITextRange *)range
{
    if (![range isKindOfClass:[TermRange class]] || !self.vt) return nil;
    NSInteger s = ((TermPosition *)range.start).offset;
    NSInteger e = ((TermPosition *)range.end).offset;
    if (e <= s) return @"";
    int r0, c0, r1, c1;
    [self offsetToRow:&r0 col:&c0 offset:s];
    [self offsetToRow:&r1 col:&c1 offset:e - 1];
    char *t = vt_range_text(self.vt, r0, c0, r1, c1);
    NSString *str = t ? [NSString stringWithUTF8String:t] : nil;
    free(t);
    return str ?: @"";
}

- (void)setSelectedTextRange:(UITextRange *)range
{
    if (![range isKindOfClass:[TermRange class]]) return;
    NSInteger s = ((TermPosition *)range.start).offset;
    NSInteger e = ((TermPosition *)range.end).offset;
    if (e > s) {
        int r0, c0, r1, c1;
        [self offsetToRow:&r0 col:&c0 offset:s];
        [self offsetToRow:&r1 col:&c1 offset:e - 1];
        [self setSelectionFromRow:r0 col:c0 toRow:r1 col:c1];
        return;
    }
    if (_hasSelection) {
        _hasSelection = NO;
        [self setNeedsDisplay];
    }
    /*
     * 这里故意"什么都不做"。
     * 系统有时候会要求把光标挪到某个位置（键盘双指拖、听写、辅助功能），
     * 唯一能反映到 shell 的办法是发方向键，但那种请求并不总能和真实操作
     * 区分开，发错了就是凭空往命令行里插方向键。所以光标移动只走显式按键：
     * 快捷键条上的方向键 + 硬件键盘。
     */
}

- (UITextRange *)selectedTextRange
{
    if (_hasSelection) {
        NSInteger r0, c0, r1, c1;
        [self orderedSelRow0:&r0 col0:&c0 row1:&r1 col1:&c1];
        NSInteger s = r0 * TERM_ROW_STRIDE + c0;
        NSInteger e = r1 * TERM_ROW_STRIDE + c1 + 1;
        return [self rangeFrom:s to:e];
    }
    NSInteger o = [self caretOffset];
    return [self rangeFrom:o to:o];
}

- (UITextRange *)markedTextRange
{
    if (!_markedText.length) return nil;
    NSInteger c = [self caretOffset];
    return [self rangeFrom:c to:c + (NSInteger)[self cellWidthOfString:_markedText]];
}

- (void)setMarkedTextStyle:(NSDictionary *)style { _markedTextStyle = style; }
- (NSDictionary *)markedTextStyle { return _markedTextStyle; }

- (void)setMarkedText:(NSString *)text selectedRange:(NSRange)sel
{
    _markedText = [text copy];
    _markedSel = sel;
    [self setNeedsDisplay];
    [_inputDelegate selectionWillChange:self];
    [_inputDelegate selectionDidChange:self];
    if (!_markedText.length) [self unmarkText];
}

- (void)unmarkText
{
    if (!_markedText.length) return;
    _markedText = nil;
    _markedSel = NSMakeRange(0, 0);
    [self setNeedsDisplay];
}

- (void)replaceRange:(UITextRange *)range withText:(NSString *)text
{
    NSInteger s = ((TermPosition *)range.start).offset;
    NSInteger e = ((TermPosition *)range.end).offset;
    if (!text.length && e > s) {
        int r0, c0, r1, c1;
        [self offsetToRow:&r0 col:&c0 offset:s];
        [self offsetToRow:&r1 col:&c1 offset:e - 1];
        NSInteger curRow = [self baseGlobalRow] + vt_cursor_y(self.vt);
        if (r0 == r1 && r1 == curRow && c1 + 1 == (NSInteger)vt_cursor_x(self.vt)) {
            /* 选中的正好是光标前面那一段，退格删掉 */
            [self clearSelection];
            for (NSInteger i = 0; i < c1 - c0 + 1; i++) [self sendKey:VK_BACKSPACE mods:0 ch:0];
            return;
        }
        [self clearSelection];
        return;
    }
    if (text.length) [self insertText:text];
}

- (void)insertText:(NSString *)text
{
    if (!text.length || !self.vt) return;
    if (_markedText.length) {
        _markedText = nil;
        _markedSel = NSMakeRange(0, 0);
    }
    [self resetBlink];
    [self scrollToBottom];
    if (text.length == 1) {
        unichar u = [text characterAtIndex:0];
        if (u == '\n' || u == '\r') { [self sendKey:VK_ENTER mods:0 ch:0]; return; }
        if (u == '\t') { [self sendKey:VK_TAB mods:0 ch:0]; return; }
        if (u == 0x7F || u == 0x08) { [self sendKey:VK_BACKSPACE mods:0 ch:0]; return; }
        [self sendKey:VK_CHAR mods:0 ch:(uint32_t)u];
        return;
    }
    /* 中文候选上屏、听写、手写这些都是一把给整串，直接原样送 */
    [self sendString:text];
    [self consumeStickyModifiers];
    [self setNeedsDisplay];
}

- (void)deleteBackward
{
    if (_markedText.length) {
        _markedText = nil;
        _markedSel = NSMakeRange(0, 0);
        [self setNeedsDisplay];
        return;
    }
    if (_hasSelection) {
        NSInteger r0, c0, r1, c1;
        [self orderedSelRow0:&r0 col0:&c0 row1:&r1 col1:&c1];
        NSInteger curRow = [self baseGlobalRow] + vt_cursor_y(self.vt);
        [self clearSelection];
        if (r0 == r1 && r1 == curRow) {
            NSInteger n = c1 - c0 + 1;
            for (NSInteger i = 0; i < n; i++) [self sendKey:VK_BACKSPACE mods:0 ch:0];
        }
        return;
    }
    [self sendKey:VK_BACKSPACE mods:0 ch:0];
}

- (BOOL)hasText { return self.vt != NULL; }

- (NSComparisonResult)comparePosition:(UITextPosition *)pos toPosition:(UITextPosition *)other
{
    NSInteger a = ((TermPosition *)pos).offset;
    NSInteger b = ((TermPosition *)other).offset;
    if (a < b) return NSOrderedAscending;
    if (a > b) return NSOrderedDescending;
    return NSOrderedSame;
}

- (NSInteger)offsetFromPosition:(UITextPosition *)from toPosition:(UITextPosition *)to
{
    return ((TermPosition *)to).offset - ((TermPosition *)from).offset;
}

- (UITextPosition *)positionWithinRange:(UITextRange *)range
                    farthestInDirection:(UITextLayoutDirection)dir
{
    if (![range isKindOfClass:[TermRange class]]) return nil;
    if (dir == UITextLayoutDirectionLeft || dir == UITextLayoutDirectionUp) return range.start;
    return range.end;
}

- (NSWritingDirection)baseWritingDirectionForPosition:(UITextPosition *)pos
                                          inDirection:(UITextStorageDirection)dir
{
    return NSWritingDirectionLeftToRight;
}

- (id<UITextInputTokenizer>)tokenizer { return _tokenizer; }

- (void)setInputDelegate:(id<UITextInputDelegate>)d { _inputDelegate = d; }
- (id<UITextInputDelegate>)inputDelegate { return _inputDelegate; }

- (UITextStorageDirection)selectionAffinity { return UITextStorageDirectionForward; }
- (void)setSelectionAffinity:(UITextStorageDirection)a { }

- (void)setBaseWritingDirection:(UITextWritingDirection)d forRange:(UITextRange *)r { }

- (CGRect)caretRectForPosition:(UITextPosition *)pos
{
    if (![pos isKindOfClass:[TermPosition class]] || !self.vt) return CGRectZero;
    int row, col;
    [self offsetToRow:&row col:&col offset:((TermPosition *)pos).offset];
    NSInteger dy = (NSInteger)row - [self baseGlobalRow];
    if (dy < 0 || dy > _rows - 1) return CGRectZero;
    if (col > _cols) col = _cols;
    return [self rectForDisplayRow:dy col:col width:1];
}

- (CGRect)firstRectForRange:(UITextRange *)range
{
    if (![range isKindOfClass:[TermRange class]]) return CGRectZero;
    return [self caretRectForPosition:range.start];
}

- (NSArray<UITextSelectionRect *> *)selectionRectsForRange:(UITextRange *)range
{
    NSMutableArray *out = [NSMutableArray array];
    if (![range isKindOfClass:[TermRange class]] || !self.vt) return out;
    NSInteger s = ((TermPosition *)range.start).offset;
    NSInteger e = ((TermPosition *)range.end).offset;
    if (e <= s) return out;
    int r0, c0, r1, c1;
    [self offsetToRow:&r0 col:&c0 offset:s];
    [self offsetToRow:&r1 col:&c1 offset:e - 1];
    NSInteger base = [self baseGlobalRow];
    [[self colorForVT:self.theme->selection fallback:0x334455] setFill];
    for (NSInteger r = r0; r <= r1; r++) {
        NSInteger dy = r - base;
        if (dy < 0 || dy > _rows - 1) continue;
        VtLine *l = [self lineAtGlobalRow:r];
        NSInteger x0 = (r == r0) ? c0 : 0;
        NSInteger x1 = (r == r1) ? c1 + 1 : (l ? l->cols : 0);
        if (x1 <= x0) continue;
        TermSelectionRect *sr = [[TermSelectionRect alloc] init];
        sr.rectValue = [self rectForDisplayRow:dy col:x0 width:(int)(x1 - x0)];
        [out addObject:sr];
    }
    return out;
}
@end
