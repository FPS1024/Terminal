#import <UIKit/UIKit.h>
#import "vt.h"
#import "Settings.h"

@class TerminalView;

@protocol TerminalViewDelegate <NSObject>
- (void)terminalView:(TerminalView *)v sendBytes:(const char *)bytes length:(NSUInteger)len;
- (void)terminalView:(TerminalView *)v didResizeCols:(int)cols rows:(int)rows;
- (void)terminalViewDidScroll:(TerminalView *)v;
- (void)terminalView:(TerminalView *)v openURL:(NSURL *)url;
@optional
/* 粘贴内容含换行等危险字符，交给控制器弹确认框 */
- (void)terminalView:(TerminalView *)v confirmPaste:(NSString *)text bracketed:(BOOL)bracketed;
/* 捏合缩放改了字号，控制器负责持久化 */
- (void)terminalView:(TerminalView *)v didChangeFontSize:(CGFloat)size;
/* 一次性 Ctrl/Alt 用掉了，控制器把高亮取消 */
- (void)terminalViewDidChangeStickyModifiers:(TerminalView *)v;
@end

@interface TerminalView : UIView <UITextInput, UITextInputTraits>
@property (nonatomic, weak) id<TerminalViewDelegate> delegate;
@property (nonatomic, assign) Vt *vt;
@property (nonatomic, assign) const TermTheme *theme;
@property (nonatomic, copy) NSString *fontName;
@property (nonatomic, assign) CGFloat fontSize;
@property (nonatomic, assign) BOOL cursorBlink;
@property (nonatomic, assign) BOOL boldAsBright;
@property (nonatomic, assign) BOOL sendMouseEvents;
@property (nonatomic, assign) BOOL confirmMultilinePaste;
@property (nonatomic, assign) BOOL bracketPaste;
/* 工具栏上的 Ctrl / Alt：按下一次作用于下一个按键，用完自动弹起 */
@property (nonatomic, assign) BOOL stickyCtrl;
@property (nonatomic, assign) BOOL stickyAlt;
@property (nonatomic, readonly) int cols;
@property (nonatomic, readonly) int rows;
@property (nonatomic, readonly) CGFloat cellWidth;
@property (nonatomic, readonly) CGFloat cellHeight;
@property (nonatomic, readonly) BOOL hasMarkedText;
/* 键盘上方的快捷键条，由控制器塞进来 */
@property (nonatomic, strong) UIView *keyBar;

- (void)reloadAppearance;
- (void)terminalDidUpdate;           /* 核心有新内容，重绘 */
- (void)scrollToBottom;
- (void)scrollToTop;
- (void)scrollByLines:(NSInteger)lines;
- (BOOL)hasSelection;
- (NSString *)selectedText;
- (void)clearSelection;
- (void)selectAll;
- (void)copySelection;
- (void)pasteString:(NSString *)text withBrackets:(BOOL)brackets;
- (void)pasteFromPasteboard;
- (void)showEditMenu;
- (void)sendKey:(int)key mods:(int)mods ch:(uint32_t)ch;
- (void)sendString:(NSString *)str;
- (void)resetBlink;
@end
