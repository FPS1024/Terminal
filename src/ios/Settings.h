#import <UIKit/UIKit.h>
#import <stdint.h>

/* 主题：颜色都用 vt.h 里的编码(0x01|索引 / 0x02|RGB / 0xFFFFFFFF 表示默认) */
typedef struct {
    const char *name;
    const char *displayName;
    uint32_t fg, bg, cursor, selection, bold;
    uint32_t ansi[16];
} TermTheme;

/* 所有内置主题，最后一个元素的 name 为 NULL */
const TermTheme *TermThemes(void);
const TermTheme *TermThemeNamed(const char *name);

/* 依赖当前主题与调色板的取色 */
UIColor *TermColorFor(uint32_t color, const TermTheme *theme, uint32_t fallback, uint32_t *palette);

@interface TermSettings : NSObject
+ (instancetype)shared;
@property (nonatomic) CGFloat fontSize;
@property (nonatomic, copy) NSString *fontName;
@property (nonatomic, copy) NSString *themeName;
@property (nonatomic) NSInteger scrollbackLines;
@property (nonatomic, copy) NSString *shellPath;
@property (nonatomic, copy) NSString *startupCommand;      /* 启动后自动执行 */
@property (nonatomic) BOOL bellHaptic;
@property (nonatomic) BOOL bellSound;
@property (nonatomic) BOOL confirmMultilinePaste;
@property (nonatomic) BOOL bracketPaste;                   /* 粘贴时加 \e[200~..\e[201~ */
@property (nonatomic) NSInteger cursorStyle;               /* 0=跟随 1=方块 2=竖线 3=下划线 */
@property (nonatomic) BOOL cursorBlink;
@property (nonatomic) BOOL asciiKeyboard;                  /* 键盘类型：ASCII 还是默认(带中文候选条) */
@property (nonatomic) BOOL hideStatusBar;
@property (nonatomic) BOOL boldAsBright;
@property (nonatomic) BOOL selectOnLongPress;
- (void)reload;
- (void)persist;
/* 读取 ~/.terminalrc (key=value)，覆盖默认值 */
- (void)loadConfigFile;
@end
