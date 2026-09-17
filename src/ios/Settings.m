#import "Settings.h"
#import "platform.h"
#import <stdio.h>
#import <stdlib.h>
#import <string.h>

/* ---------------- 主题 ---------------- */
#define RGB(r,g,b) (0x02000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define IDX(i)     (0x01000000u | (uint32_t)(i))

static const TermTheme kThemes[] = {
    {
        "dark", "深色",
        RGB(0xE6,0xE6,0xE6), RGB(0x1B,0x1D,0x1E), RGB(0x7B,0xD8,0x8F), RGB(0x33,0x3A,0x55), RGB(0xFF,0xFF,0xFF),
        { RGB(0x3B,0x40,0x48), RGB(0xE0,0x6C,0x75), RGB(0x98,0xC3,0x79), RGB(0xE5,0xC0,0x7B),
          RGB(0x61,0xAF,0xEF), RGB(0xC6,0x78,0xDD), RGB(0x56,0xB6,0xC2), RGB(0xAB,0xB2,0xBF),
          RGB(0x5C,0x63,0x70), RGB(0xFF,0x6E,0x77), RGB(0xA6,0xE3,0x7D), RGB(0xFF,0xD8,0x7F),
          RGB(0x6F,0xBC,0xFF), RGB(0xD5,0x8A,0xEE), RGB(0x62,0xCB,0xD8), RGB(0xFF,0xFF,0xFF) }
    },
    {
        "light", "浅色",
        RGB(0x24,0x29,0x2E), RGB(0xFF,0xFF,0xFF), RGB(0x00,0x7A,0xCC), RGB(0xB4,0xD5,0xFE), RGB(0x00,0x00,0x00),
        { RGB(0x24,0x29,0x2E), RGB(0xCF,0x22,0x2E), RGB(0x11,0x67,0x1E), RGB(0x79,0x60,0x00),
          RGB(0x0A,0x30,0x69), RGB(0x82,0x50,0xDF), RGB(0x0E,0x71,0x8C), RGB(0x6A,0x73,0x7D),
          RGB(0x6A,0x73,0x7D), RGB(0xA4,0x00,0x2B), RGB(0x14,0x41,0x1D), RGB(0x98,0x79,0x00),
          RGB(0x00,0x54,0xA6), RGB(0x6F,0x42,0xC1), RGB(0x1B,0x7C,0x99), RGB(0xFF,0xFF,0xFF) }
    },
    {
        "solarized-dark", "Solarized 深色",
        RGB(0x83,0x94,0x96), RGB(0x00,0x2B,0x36), RGB(0x93,0xA1,0xA1), RGB(0x07,0x36,0x42), RGB(0xFF,0xFF,0xFF),
        { RGB(0x07,0x36,0x42), RGB(0xDC,0x32,0x2F), RGB(0x85,0x99,0x00), RGB(0xB5,0x89,0x00),
          RGB(0x26,0x8B,0xD2), RGB(0xD3,0x36,0x82), RGB(0x2A,0xA1,0x98), RGB(0xEE,0xE8,0xD5),
          RGB(0x00,0x2B,0x36), RGB(0xCB,0x4B,0x16), RGB(0x58,0x6E,0x75), RGB(0x65,0x7B,0x83),
          RGB(0x83,0x94,0x96), RGB(0x6C,0x71,0xC4), RGB(0x93,0xA1,0xA1), RGB(0xFD,0xF6,0xE3) }
    },
    {
        "dracula", "Dracula",
        RGB(0xF8,0xF8,0xF2), RGB(0x28,0x2A,0x36), RGB(0xF8,0xF8,0xF2), RGB(0x44,0x47,0x5A), RGB(0xFF,0xFF,0xFF),
        { RGB(0x21,0x22,0x2C), RGB(0xFF,0x55,0x55), RGB(0x50,0xFA,0x7B), RGB(0xF1,0xFA,0x8C),
          RGB(0xBD,0x93,0xF9), RGB(0xFF,0x79,0xC6), RGB(0x8B,0xE9,0xFD), RGB(0xF8,0xF8,0xF2),
          RGB(0x62,0x72,0xA4), RGB(0xFF,0x6E,0x6E), RGB(0x69,0xFF,0x94), RGB(0xFF,0xFF,0xA5),
          RGB(0xD6,0xAC,0xFF), RGB(0xFF,0x92,0xDF), RGB(0xA4,0xFF,0xFF), RGB(0xFF,0xFF,0xFF) }
    },
    {
        "classic", "经典黑白",
        RGB(0xFF,0xFF,0xFF), RGB(0x00,0x00,0x00), RGB(0xFF,0xFF,0xFF), RGB(0x44,0x44,0x44), RGB(0xFF,0xFF,0xFF),
        { RGB(0x00,0x00,0x00), RGB(0xCD,0x00,0x00), RGB(0x00,0xCD,0x00), RGB(0xCD,0xCD,0x00),
          RGB(0x00,0x00,0xEE), RGB(0xCD,0x00,0xCD), RGB(0x00,0xCD,0xCD), RGB(0xE5,0xE5,0xE5),
          RGB(0x7F,0x7F,0x7F), RGB(0xFF,0x00,0x00), RGB(0x00,0xFF,0x00), RGB(0xFF,0xFF,0x00),
          RGB(0x5C,0x5C,0xFF), RGB(0xFF,0x00,0xFF), RGB(0x00,0xFF,0xFF), RGB(0xFF,0xFF,0xFF) }
    },
    { NULL, NULL, 0, 0, 0, 0, 0, { 0 } }
};

const TermTheme *TermThemes(void) { return kThemes; }

const TermTheme *TermThemeNamed(const char *name)
{
    if (!name) return &kThemes[0];
    for (int i = 0; kThemes[i].name; i++)
        if (strcmp(kThemes[i].name, name) == 0) return &kThemes[i];
    return &kThemes[0];
}

UIColor *TermColorFor(uint32_t color, const TermTheme *theme, uint32_t fallback, uint32_t *palette)
{
    uint32_t c = (color == 0xFFFFFFFFu) ? fallback : color;
    if (c == 0xFFFFFFFFu) c = 0xFFFFFFu;
    if ((c >> 24) == 0x01u) {
        int idx = (int)(c & 0xFF);
        uint32_t v = palette ? palette[idx] : 0;
        if ((v >> 24) != 0x02u) v = (idx < 16) ? theme->ansi[idx] : 0xCCCCCCu;
        c = v;
    }
    return [UIColor colorWithRed:((c >> 16) & 0xFF) / 255.0
                           green:((c >> 8) & 0xFF) / 255.0
                            blue:(c & 0xFF) / 255.0 alpha:1.0];
}

/* ---------------- 设置 ---------------- */
@implementation TermSettings

+ (instancetype)shared
{
    static TermSettings *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [[TermSettings alloc] init]; });
    return s;
}

- (instancetype)init
{
    self = [super init];
    if (self) {
        _fontSize = 14.0;
        _fontName = @"Menlo";
        _themeName = @"dark";
        _scrollbackLines = 5000;
        _shellPath = @"";
        _startupCommand = @"";
        _bellHaptic = YES;
        _bellSound = NO;
        _confirmMultilinePaste = YES;
        _bracketPaste = YES;
        _cursorStyle = 0;
        _cursorBlink = YES;
        /* 默认给"带中文候选条"的键盘；想要纯 ASCII 布局可以在设置里开 */
        _asciiKeyboard = NO;
        _hideStatusBar = NO;
        _boldAsBright = YES;
        _selectOnLongPress = YES;
        [self reload];
        [self loadConfigFile];
    }
    return self;
}

- (void)reload
{
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    if ([d objectForKey:@"fontSize"]) _fontSize = [d floatForKey:@"fontSize"];
    if ([d objectForKey:@"fontName"]) _fontName = [d stringForKey:@"fontName"];
    if ([d objectForKey:@"themeName"]) _themeName = [d stringForKey:@"themeName"];
    if ([d objectForKey:@"scrollbackLines"]) _scrollbackLines = [d integerForKey:@"scrollbackLines"];
    if ([d objectForKey:@"shellPath"]) _shellPath = [d stringForKey:@"shellPath"];
    if ([d objectForKey:@"startupCommand"]) _startupCommand = [d stringForKey:@"startupCommand"];
    if ([d objectForKey:@"bellHaptic"]) _bellHaptic = [d boolForKey:@"bellHaptic"];
    if ([d objectForKey:@"bellSound"]) _bellSound = [d boolForKey:@"bellSound"];
    if ([d objectForKey:@"confirmMultilinePaste"]) _confirmMultilinePaste = [d boolForKey:@"confirmMultilinePaste"];
    if ([d objectForKey:@"bracketPaste"]) _bracketPaste = [d boolForKey:@"bracketPaste"];
    if ([d objectForKey:@"cursorStyle"]) _cursorStyle = [d integerForKey:@"cursorStyle"];
    if ([d objectForKey:@"cursorBlink"]) _cursorBlink = [d boolForKey:@"cursorBlink"];
    if ([d objectForKey:@"asciiKeyboard"]) _asciiKeyboard = [d boolForKey:@"asciiKeyboard"];
    if ([d objectForKey:@"hideStatusBar"]) _hideStatusBar = [d boolForKey:@"hideStatusBar"];
    if ([d objectForKey:@"boldAsBright"]) _boldAsBright = [d boolForKey:@"boldAsBright"];
    if ([d objectForKey:@"selectOnLongPress"]) _selectOnLongPress = [d boolForKey:@"selectOnLongPress"];
}

- (void)persist
{
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    [d setFloat:_fontSize forKey:@"fontSize"];
    [d setObject:_fontName forKey:@"fontName"];
    [d setObject:_themeName forKey:@"themeName"];
    [d setInteger:_scrollbackLines forKey:@"scrollbackLines"];
    [d setObject:_shellPath forKey:@"shellPath"];
    [d setObject:_startupCommand forKey:@"startupCommand"];
    [d setBool:_bellHaptic forKey:@"bellHaptic"];
    [d setBool:_bellSound forKey:@"bellSound"];
    [d setBool:_confirmMultilinePaste forKey:@"confirmMultilinePaste"];
    [d setBool:_bracketPaste forKey:@"bracketPaste"];
    [d setInteger:_cursorStyle forKey:@"cursorStyle"];
    [d setBool:_cursorBlink forKey:@"cursorBlink"];
    [d setBool:_asciiKeyboard forKey:@"asciiKeyboard"];
    [d setBool:_hideStatusBar forKey:@"hideStatusBar"];
    [d setBool:_boldAsBright forKey:@"boldAsBright"];
    [d setBool:_selectOnLongPress forKey:@"selectOnLongPress"];
    [d synchronize];
}

- (void)loadConfigFile
{
    char path[512];
    jb_path(path, sizeof(path), "");
    NSString *home = NSHomeDirectory();
    NSString *p = [home stringByAppendingPathComponent:@".terminalrc"];
    FILE *fp = fopen([p UTF8String], "r");
    if (!fp) return;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line;
        char *v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        char *ke = k + strlen(k);
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        while (*v == ' ' || *v == '\t') v++;
        char *ve = v + strlen(v);
        while (ve > v && (ve[-1] == '\n' || ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) *--ve = 0;
        if (!k[0]) continue;
        if (strcmp(k, "font_size") == 0) _fontSize = atof(v);
        else if (strcmp(k, "font") == 0) _fontName = [NSString stringWithUTF8String:v];
        else if (strcmp(k, "theme") == 0) _themeName = [NSString stringWithUTF8String:v];
        else if (strcmp(k, "scrollback") == 0) _scrollbackLines = atoi(v);
        else if (strcmp(k, "shell") == 0) _shellPath = [NSString stringWithUTF8String:v];
        else if (strcmp(k, "startup") == 0) _startupCommand = [NSString stringWithUTF8String:v];
        else if (strcmp(k, "cursor") == 0) _cursorStyle = atoi(v);
        else if (strcmp(k, "cursor_blink") == 0) _cursorBlink = atoi(v) ? YES : NO;
        else if (strcmp(k, "bell_haptic") == 0) _bellHaptic = atoi(v) ? YES : NO;
        else if (strcmp(k, "bell_sound") == 0) _bellSound = atoi(v) ? YES : NO;
        else if (strcmp(k, "ascii_keyboard") == 0) _asciiKeyboard = atoi(v) ? YES : NO;
        else if (strcmp(k, "bold_as_bright") == 0) _boldAsBright = atoi(v) ? YES : NO;
        else if (strcmp(k, "confirm_paste") == 0) _confirmMultilinePaste = atoi(v) ? YES : NO;
    }
    fclose(fp);
}

@end
