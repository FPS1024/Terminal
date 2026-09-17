#import "TerminalViewController.h"
#import "TerminalSession.h"
#import "TerminalView.h"
#import "Settings.h"
#import "SettingsViewController.h"
#import "platform.h"
#import <AudioToolbox/AudioToolbox.h>

@interface TerminalViewController () <TerminalViewDelegate, TerminalSessionDelegate>
@end

@implementation TerminalViewController {
    TerminalView   *_term;
    UIView         *_topBar;
    UILabel        *_titleLabel;
    UIButton       *_newButton, *_kbButton, *_gearButton, *_closeButton;
    UIScrollView   *_tabScroll;
    NSMutableArray *_tabButtons;
    NSMutableArray *_sessions;
    NSInteger       _current;
    UIView         *_keyBar;
    UIButton       *_ctrlButton, *_altButton;
    CGFloat         _keyboardHeight;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    for (TerminalSession *s in _sessions) [s terminate];
}

- (BOOL)prefersStatusBarHidden { return [TermSettings shared].hideStatusBar; }

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];
    _sessions = [NSMutableArray array];
    _tabButtons = [NSMutableArray array];

    _topBar = [[UIView alloc] initWithFrame:CGRectZero];
    _topBar.backgroundColor = [UIColor colorWithWhite:0.13 alpha:1.0];
    [self.view addSubview:_topBar];

    _titleLabel = [[UILabel alloc] initWithFrame:CGRectZero];
    _titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
    _titleLabel.textColor = [UIColor colorWithWhite:0.92 alpha:1.0];
    _titleLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [_topBar addSubview:_titleLabel];

    _newButton = [self barButton:@"＋" action:@selector(newSession)];
    _closeButton = [self barButton:@"✕" action:@selector(confirmCloseSession)];
    _kbButton = [self barButton:@"⌨" action:@selector(toggleKeyboard)];
    _gearButton = [self barButton:@"⚙" action:@selector(openSettings)];
    [_topBar addSubview:_newButton];
    [_topBar addSubview:_closeButton];
    [_topBar addSubview:_kbButton];
    [_topBar addSubview:_gearButton];

    _tabScroll = [[UIScrollView alloc] initWithFrame:CGRectZero];
    _tabScroll.showsHorizontalScrollIndicator = NO;
    _tabScroll.hidden = YES;
    [_topBar addSubview:_tabScroll];

    _term = [[TerminalView alloc] initWithFrame:CGRectZero];
    _term.delegate = self;
    _term.theme = TermThemeNamed([[TermSettings shared].themeName UTF8String]);
    [self.view addSubview:_term];

    _keyBar = [self buildKeyBar];
    _term.keyBar = _keyBar;

    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(keyboardWillChange:)
                                                 name:UIKeyboardWillChangeFrameNotification
                                               object:nil];

    [self applySettings];
    [self newSession];
}

/* ---------------- 顶栏 / 快捷键条 ---------------- */

- (UIButton *)barButton:(NSString *)title action:(SEL)action
{
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:title forState:UIControlStateNormal];
    b.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightMedium];
    [b setTitleColor:[UIColor colorWithWhite:0.88 alpha:1.0] forState:UIControlStateNormal];
    b.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.10];
    b.layer.cornerRadius = 8;
    [b addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return b;
}

- (UIButton *)keyButton:(NSString *)title action:(SEL)action in:(UIView *)parent x:(CGFloat *)x
{
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:title forState:UIControlStateNormal];
    b.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
    [b setTitleColor:[UIColor colorWithWhite:0.93 alpha:1.0] forState:UIControlStateNormal];
    b.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.10];
    b.layer.cornerRadius = 6;
    CGSize sz = [title sizeWithAttributes:@{ NSFontAttributeName: b.titleLabel.font }];
    CGFloat w = MAX(40.0, ceil(sz.width) + 22.0);
    b.frame = CGRectMake(*x, 6, w, 32);
    [b addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    [parent addSubview:b];
    *x = CGRectGetMaxX(b.frame) + 6;
    return b;
}

- (UIView *)buildKeyBar
{
    CGFloat w = MAX(320.0, self.view.bounds.size.width);
    UIView *bar = [[UIView alloc] initWithFrame:CGRectMake(0, 0, w, 44)];
    bar.backgroundColor = [UIColor colorWithWhite:0.15 alpha:1.0];
    bar.autoresizingMask = UIViewAutoresizingFlexibleWidth;

    UIScrollView *sv = [[UIScrollView alloc] initWithFrame:bar.bounds];
    sv.showsHorizontalScrollIndicator = NO;
    sv.alwaysBounceHorizontal = YES;
    sv.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [bar addSubview:sv];

    CGFloat x = 6;
    [self keyButton:@"esc" action:@selector(keyEsc) in:sv x:&x];
    [self keyButton:@"tab" action:@selector(keyTab) in:sv x:&x];
    [self keyButton:@"⇧tab" action:@selector(keyShiftTab) in:sv x:&x];
    _ctrlButton = [self keyButton:@"ctrl" action:@selector(keyCtrl) in:sv x:&x];
    _altButton = [self keyButton:@"alt" action:@selector(keyAlt) in:sv x:&x];
    [self keyButton:@"^C" action:@selector(keyInterrupt) in:sv x:&x];
    [self keyButton:@"^D" action:@selector(keyEOF) in:sv x:&x];
    [self keyButton:@"^Z" action:@selector(keySuspend) in:sv x:&x];
    [self keyButton:@"←" action:@selector(keyLeft) in:sv x:&x];
    [self keyButton:@"↑" action:@selector(keyUp) in:sv x:&x];
    [self keyButton:@"↓" action:@selector(keyDown) in:sv x:&x];
    [self keyButton:@"→" action:@selector(keyRight) in:sv x:&x];
    [self keyButton:@"⇱" action:@selector(keyHome) in:sv x:&x];
    [self keyButton:@"⇲" action:@selector(keyEnd) in:sv x:&x];
    [self keyButton:@"pgup" action:@selector(keyPageUp) in:sv x:&x];
    [self keyButton:@"pgdn" action:@selector(keyPageDown) in:sv x:&x];
    [self keyButton:@"del" action:@selector(keyDelete) in:sv x:&x];

    NSArray *symbols = @[ @"-", @"/", @"|", @"~", @"*", @"$", @"&", @"\"", @"'", @"`", @":", @"=" ];
    for (NSString *sym in symbols) {
        UIButton *b = [self keyButton:sym action:@selector(keySymbol:) in:sv x:&x];
        b.tag = (NSInteger)[sym characterAtIndex:0];
    }

    [self keyButton:@"粘贴" action:@selector(keyPaste) in:sv x:&x];
    [self keyButton:@"复制" action:@selector(keyCopy) in:sv x:&x];
    [self keyButton:@"清屏" action:@selector(keyClear) in:sv x:&x];
    [self keyButton:@"⌄" action:@selector(toggleKeyboard) in:sv x:&x];
    sv.contentSize = CGSizeMake(x + 6, 44);
    [self refreshModButtons];
    return bar;
}

- (void)refreshModButtons
{
    UIColor *on = [UIColor colorWithRed:0.30 green:0.55 blue:0.95 alpha:1.0];
    UIColor *off = [UIColor colorWithWhite:1.0 alpha:0.10];
    _ctrlButton.backgroundColor = _term.stickyCtrl ? on : off;
    _altButton.backgroundColor = _term.stickyAlt ? on : off;
}

/* ---------------- 布局 ---------------- */

- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    [self layoutBars];
}

- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
    [self layoutBars];
    if (![_term isFirstResponder]) [_term becomeFirstResponder];
}

- (void)viewWillTransitionToSize:(CGSize)size withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator
{
    [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
    [coordinator animateAlongsideTransition:^(id<UIViewControllerTransitionCoordinatorContext> ctx) {
        [self layoutBars];
    } completion:^(id<UIViewControllerTransitionCoordinatorContext> ctx) {
        [self layoutBars];
    }];
}

- (void)layoutBars
{
    CGFloat w = self.view.bounds.size.width;
    CGFloat h = self.view.bounds.size.height;
    UIEdgeInsets ins = UIEdgeInsetsZero;
    if ([self.view respondsToSelector:@selector(safeAreaInsets)])
        ins = self.view.safeAreaInsets;

    CGFloat btn = 34;
    CGFloat pad = 8;
    CGFloat gearX = w - pad - btn;
    CGFloat kbX = gearX - pad - btn;
    CGFloat newX = kbX - pad - btn;
    CGFloat closeX = newX - pad - btn;
    CGFloat rowH = 40;

    CGFloat tabH = _tabScroll.hidden ? 0 : 32;
    _topBar.frame = CGRectMake(0, 0, w, ins.top + rowH + tabH);
    _gearButton.frame = CGRectMake(gearX, ins.top + 3, btn, btn);
    _kbButton.frame = CGRectMake(kbX, ins.top + 3, btn, btn);
    _newButton.frame = CGRectMake(newX, ins.top + 3, btn, btn);
    _closeButton.frame = CGRectMake(closeX, ins.top + 3, btn, btn);
    _titleLabel.frame = CGRectMake(12, ins.top, MAX(0, closeX - 16), rowH);
    _tabScroll.frame = CGRectMake(0, ins.top + rowH, w, tabH);
    [self layoutTabs];

    CGFloat bottom = _keyboardHeight > 0 ? _keyboardHeight : ins.bottom;
    if (bottom > h * 0.7) bottom = ins.bottom;   /* 转屏后残留的旧键盘高度，别把终端压没了 */
    CGFloat top = CGRectGetMaxY(_topBar.frame);
    _term.frame = CGRectMake(0, top, w, MAX(0, h - top - bottom));

    CGRect kb = _keyBar.frame;
    kb.size.width = w;
    _keyBar.frame = kb;
}

- (void)layoutTabs
{
    CGFloat x = 8;
    for (UIButton *b in _tabButtons) {
        CGSize sz = [b.titleLabel.text sizeWithAttributes:@{ NSFontAttributeName: b.titleLabel.font }];
        CGFloat w = MIN(200.0, MAX(56.0, ceil(sz.width) + 22.0));
        b.frame = CGRectMake(x, 3, w, 26);
        x = CGRectGetMaxX(b.frame) + 6;
    }
    _tabScroll.contentSize = CGSizeMake(x + 6, 32);
}

- (void)keyboardWillChange:(NSNotification *)n
{
    CGRect end = [n.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    CGRect inView = [self.view convertRect:end fromView:nil];
    CGFloat overlap = CGRectGetMaxY(self.view.bounds) - CGRectGetMinY(inView);
    if (overlap < 0) overlap = 0;
    if (fabs(overlap - _keyboardHeight) < 0.5) return;
    _keyboardHeight = overlap;
    NSTimeInterval dur = [n.userInfo[UIKeyboardAnimationDurationUserInfoKey] doubleValue];
    [UIView animateWithDuration:dur animations:^{ [self layoutBars]; }];
}

- (void)toggleKeyboard
{
    if ([_term isFirstResponder]) [_term resignFirstResponder];
    else [_term becomeFirstResponder];
}

/* ---------------- 会话 ---------------- */

- (void)newSession
{
    int cols = _term.cols > 1 ? _term.cols : 80;
    int rows = _term.rows > 1 ? _term.rows : 24;
    TerminalSession *s = [[TerminalSession alloc] initWithCols:cols rows:rows];
    s.delegate = self;
    [_sessions addObject:s];
    _current = (NSInteger)_sessions.count - 1;
    [self attachCurrentSession];
    [s start];
    [self rebuildTabs];
    [self refreshChrome];
}

- (void)closeCurrentSession
{
    if (_current < 0 || _current >= (NSInteger)_sessions.count) return;
    /* 先把 View 和这个 vt 断开，否则等一下 session 释放了就是野指针 */
    _term.vt = NULL;
    TerminalSession *s = _sessions[_current];
    [s terminate];
    [_sessions removeObjectAtIndex:_current];
    if (_sessions.count == 0) {
        _current = -1;
        [self newSession];
        return;
    }
    if (_current >= (NSInteger)_sessions.count) _current = (NSInteger)_sessions.count - 1;
    [self attachCurrentSession];
    [self rebuildTabs];
    [self refreshChrome];
}

- (void)confirmCloseSession
{
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"关闭当前会话？"
                                                              message:@"会话里跑着的程序会被一起结束。"
                                                       preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [a addAction:[UIAlertAction actionWithTitle:@"关闭" style:UIAlertActionStyleDestructive
                                       handler:^(UIAlertAction *act) { [self closeCurrentSession]; }]];
    [self presentViewController:a animated:YES completion:nil];
}

- (void)selectSession:(UIButton *)b
{
    NSInteger idx = b.tag;
    if (idx < 0 || idx >= (NSInteger)_sessions.count || idx == _current) return;
    _current = idx;
    [self attachCurrentSession];
    [self refreshChrome];
}

- (void)attachCurrentSession
{
    if (_current < 0 || _current >= (NSInteger)_sessions.count) return;
    TerminalSession *s = _sessions[_current];
    _term.vt = s.vt;
    [self applySessionSettings:s];
    int cols = _term.cols > 1 ? _term.cols : 80;
    int rows = _term.rows > 1 ? _term.rows : 24;
    [s resizeToCols:cols rows:rows];
    [s setPixelSize:(int)_term.bounds.size.width height:(int)_term.bounds.size.height];
    [_term clearSelection];
    [_term scrollToBottom];
    [_term terminalDidUpdate];
    [_term setNeedsDisplay];
}

- (void)applySessionSettings:(TerminalSession *)s
{
    if (!s.vt) return;
    TermSettings *st = [TermSettings shared];
    vt_set_scrollback_limit(s.vt, (int)st.scrollbackLines);
    if (st.cursorStyle > 0) s.vt->cursor_style = (int)st.cursorStyle;
    [s setPixelSize:(int)_term.bounds.size.width height:(int)_term.bounds.size.height];
}

- (void)rebuildTabs
{
    for (UIButton *b in _tabButtons) [b removeFromSuperview];
    [_tabButtons removeAllObjects];
    if (_sessions.count < 2) {
        _tabScroll.hidden = YES;
    } else {
        _tabScroll.hidden = NO;
        for (NSInteger i = 0; i < (NSInteger)_sessions.count; i++) {
            TerminalSession *s = _sessions[i];
            NSString *t = s.title.length ? s.title : @"终端";
            UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
            [b setTitle:[NSString stringWithFormat:@"%ld %@", (long)i + 1, t] forState:UIControlStateNormal];
            b.titleLabel.font = [UIFont systemFontOfSize:12];
            b.tag = i;
            b.layer.cornerRadius = 6;
            [b addTarget:self action:@selector(selectSession:) forControlEvents:UIControlEventTouchUpInside];
            [_tabScroll addSubview:b];
            [_tabButtons addObject:b];
        }
    }
    [self refreshTabHighlight];
}

- (void)refreshTabHighlight
{
    for (NSInteger i = 0; i < (NSInteger)_tabButtons.count; i++) {
        UIButton *b = _tabButtons[i];
        BOOL on = (i == _current);
        b.backgroundColor = on ? [UIColor colorWithRed:0.26 green:0.48 blue:0.85 alpha:1.0]
                               : [UIColor colorWithWhite:1.0 alpha:0.09];
        [b setTitleColor:[UIColor colorWithWhite:on ? 1.0 : 0.75 alpha:1.0] forState:UIControlStateNormal];
    }
}

- (void)refreshChrome
{
    if (_current >= 0 && _current < (NSInteger)_sessions.count) {
        TerminalSession *s = _sessions[_current];
        _titleLabel.text = [NSString stringWithFormat:@"%@", s.title.length ? s.title : @"终端"];
    } else {
        _titleLabel.text = @"Terminal";
    }
    [self refreshTabHighlight];
}

/* ---------------- 设置 ---------------- */

- (void)openSettings
{
    SettingsViewController *svc = [[SettingsViewController alloc] init];
    __weak TerminalViewController *weakSelf = self;
    svc.onChanged = ^{
        TerminalViewController *me = weakSelf;
        if (me) [me applySettings];
    };
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:svc];
    [self presentViewController:nav animated:YES completion:nil];
}

- (void)applySettings
{
    TermSettings *st = [TermSettings shared];
    _term.theme = TermThemeNamed([st.themeName UTF8String]);
    _term.fontName = st.fontName;
    _term.fontSize = st.fontSize;
    _term.cursorBlink = st.cursorBlink;
    _term.boldAsBright = st.boldAsBright;
    _term.confirmMultilinePaste = st.confirmMultilinePaste;
    _term.bracketPaste = st.bracketPaste;
    _term.keyboardType = st.asciiKeyboard ? UIKeyboardTypeASCIICapable : UIKeyboardTypeDefault;
    [_term reloadInputViews];
    for (TerminalSession *s in _sessions) [self applySessionSettings:s];
    [_term reloadAppearance];
    [_term resetBlink];
    [self setNeedsStatusBarAppearanceUpdate];
    [self layoutBars];
}

/* ---------------- 快捷键动作 ---------------- */

- (void)keyEsc { [_term sendKey:VK_ESC mods:0 ch:0]; }
- (void)keyTab { [_term sendKey:VK_TAB mods:0 ch:0]; }
- (void)keyShiftTab { [_term sendKey:VK_TAB mods:VTM_SHIFT ch:0]; }
- (void)keyLeft { [_term sendKey:VK_LEFT mods:0 ch:0]; }
- (void)keyRight { [_term sendKey:VK_RIGHT mods:0 ch:0]; }
- (void)keyUp { [_term sendKey:VK_UP mods:0 ch:0]; }
- (void)keyDown { [_term sendKey:VK_DOWN mods:0 ch:0]; }
- (void)keyHome { [_term sendKey:VK_HOME mods:0 ch:0]; }
- (void)keyEnd { [_term sendKey:VK_END mods:0 ch:0]; }
- (void)keyPageUp { [_term sendKey:VK_PAGEUP mods:0 ch:0]; }
- (void)keyPageDown { [_term sendKey:VK_PAGEDOWN mods:0 ch:0]; }
- (void)keyDelete { [_term sendKey:VK_DELETE mods:0 ch:0]; }
- (void)keyInterrupt { [_term sendKey:VK_CHAR mods:VTM_CTRL ch:'c']; }
- (void)keyEOF { [_term sendKey:VK_CHAR mods:VTM_CTRL ch:'d']; }
- (void)keySuspend { [_term sendKey:VK_CHAR mods:VTM_CTRL ch:'z']; }

- (void)keyCtrl { _term.stickyCtrl = !_term.stickyCtrl; [self refreshModButtons]; }
- (void)keyAlt { _term.stickyAlt = !_term.stickyAlt; [self refreshModButtons]; }

- (void)keySymbol:(UIButton *)b
{
    [_term sendKey:VK_CHAR mods:0 ch:(uint32_t)b.tag];
    [self refreshModButtons];
}

- (void)keyPaste { [_term pasteFromPasteboard]; }
- (void)keyCopy { [_term copySelection]; }

- (void)keyClear
{
    [_term sendString:@"\x1b[H\x1b[2J\x1b[3J"];
    [_term scrollToBottom];
}

/* ---------------- TerminalViewDelegate ---------------- */

- (void)terminalView:(TerminalView *)v sendBytes:(const char *)bytes length:(NSUInteger)len
{
    if (_current < 0 || _current >= (NSInteger)_sessions.count) return;
    [_sessions[_current] sendBytes:bytes length:len];
}

- (void)terminalView:(TerminalView *)v didResizeCols:(int)cols rows:(int)rows
{
    if (_current < 0 || _current >= (NSInteger)_sessions.count) return;
    TerminalSession *s = _sessions[_current];
    [s resizeToCols:cols rows:rows];
    [s setPixelSize:(int)v.bounds.size.width height:(int)v.bounds.size.height];
}

- (void)terminalViewDidScroll:(TerminalView *)v { }

- (void)terminalView:(TerminalView *)v openURL:(NSURL *)url
{
    if (!url) return;
    UIApplication *app = [UIApplication sharedApplication];
    if ([app respondsToSelector:@selector(openURL:options:completionHandler:)]) {
        [app openURL:url options:@{} completionHandler:nil];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [app openURL:url];
#pragma clang diagnostic pop
    }
}

- (void)terminalView:(TerminalView *)v didChangeFontSize:(CGFloat)size
{
    [TermSettings shared].fontSize = size;
    [[TermSettings shared] persist];
}

- (void)terminalViewDidChangeStickyModifiers:(TerminalView *)v
{
    [self refreshModButtons];
}

- (void)terminalView:(TerminalView *)v confirmPaste:(NSString *)text bracketed:(BOOL)bracketed
{
    NSString *preview = text.length > 220 ? [[text substringToIndex:220] stringByAppendingString:@"…"] : text;
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"粘贴多行内容？"
                                                              message:preview
                                                       preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [a addAction:[UIAlertAction actionWithTitle:@"粘贴" style:UIAlertActionStyleDefault handler:^(UIAlertAction *act) {
        [self->_term pasteString:text withBrackets:bracketed];
    }]];
    [self presentViewController:a animated:YES completion:nil];
}

/* ---------------- TerminalSessionDelegate ---------------- */

- (void)sessionDidUpdate:(TerminalSession *)s
{
    if (_current >= 0 && _current < (NSInteger)_sessions.count && _sessions[_current] == s)
        [_term terminalDidUpdate];
}

- (void)session:(TerminalSession *)s didChangeTitle:(NSString *)title
{
    [self rebuildTabs];
    [self refreshChrome];
}

- (void)session:(TerminalSession *)s didExitWithStatus:(int)status
{
    [self rebuildTabs];
    [self refreshChrome];
}

- (void)sessionDidRingBell:(TerminalSession *)s
{
    TermSettings *st = [TermSettings shared];
    if (st.bellHaptic) {
        UIImpactFeedbackGenerator *g = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
        [g impactOccurred];
    }
    if (st.bellSound) AudioServicesPlaySystemSound(1104);
}

- (void)session:(TerminalSession *)s notify:(NSString *)title body:(NSString *)body
{
    if (!self.view.window) return;
    /* 越狱机上 App 大多没通知权限，直接用弹窗兜底 */
    UIAlertController *a = [UIAlertController alertControllerWithTitle:title ?: @"终端"
                                                              message:body
                                                       preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"好" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:a animated:YES completion:nil];
}

@end
