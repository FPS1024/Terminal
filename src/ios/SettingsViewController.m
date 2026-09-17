#import "SettingsViewController.h"
#import "Settings.h"
#import "platform.h"

enum {
    SecLook = 0, SecTerm, SecStart, SecAbout, SecCount
};

enum {
    RowTheme = 0, RowFont, RowFontSize, RowCursorStyle, RowCursorBlink, RowBoldBright, RowLookCount
};

enum {
    RowScrollback = 0, RowBracket, RowConfirmPaste, RowBellHaptic, RowBellSound, RowAsciiKb, RowTermCount
};

enum {
    RowShell = 0, RowStartup, RowStartCount
};

enum {
    RowVersion = 0, RowTip, RowAboutCount
};

static NSArray *FontChoices(void)
{
    return @[ @"Menlo", @"Courier", @"Courier New", @"SF Mono", @"Consolas",
              @"PingFang SC", @"Heiti SC", @"Hiragino Sans GB", @"Apple SD Gothic Neo", @"Monaco" ];
}

static NSString *CursorStyleName(NSInteger s)
{
    switch (s) {
    case 1: return @"方块";
    case 2: return @"竖线";
    case 3: return @"下划线";
    case 4: return @"闪烁下划线";
    case 5: return @"闪烁竖线";
    case 6: return @"闪烁方块";
    default: return @"跟随程序(DECSCUSR)";
    }
}

@interface SettingsViewController ()
@end

@implementation SettingsViewController

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.title = @"设置";
    self.tableView.backgroundColor = [UIColor colorWithWhite:0.10 alpha:1.0];
    self.tableView.separatorColor = [UIColor colorWithWhite:1.0 alpha:0.12];
    self.tableView.rowHeight = 46;
    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                      target:self
                                                      action:@selector(done)];
}

- (void)done
{
    [[TermSettings shared] persist];
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)changed
{
    [[TermSettings shared] persist];
    if (self.onChanged) self.onChanged();
    [self.tableView reloadData];
}

/* ---------------- 表格 ---------------- */

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tv { return SecCount; }

- (NSString *)tableView:(UITableView *)tv titleForHeaderInSection:(NSInteger)s
{
    switch (s) {
    case SecLook: return @"外观";
    case SecTerm: return @"终端";
    case SecStart: return @"启动";
    default: return @"关于";
    }
}

- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)s
{
    switch (s) {
    case SecLook: return RowLookCount;
    case SecTerm: return RowTermCount;
    case SecStart: return RowStartCount;
    default: return RowAboutCount;
    }
}

- (UITableViewCell *)cellWithStyle:(UITableViewCellStyle)style
{
    UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:style reuseIdentifier:nil];
    c.backgroundColor = [UIColor colorWithWhite:0.15 alpha:1.0];
    c.textLabel.textColor = [UIColor colorWithWhite:0.94 alpha:1.0];
    c.detailTextLabel.textColor = [UIColor colorWithWhite:0.62 alpha:1.0];
    UIView *sel = [[UIView alloc] init];
    sel.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.08];
    c.selectedBackgroundView = sel;
    return c;
}

- (UIStepper *)stepperMin:(double)mn max:(double)mx step:(double)st value:(double)v action:(SEL)a
{
    UIStepper *s = [[UIStepper alloc] init];
    s.minimumValue = mn;
    s.maximumValue = mx;
    s.stepValue = st;
    s.value = v;
    [s addTarget:self action:a forControlEvents:UIControlEventValueChanged];
    return s;
}

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip
{
    TermSettings *st = [TermSettings shared];
    UITableViewCell *c = nil;
    if (ip.section == SecLook) {
        switch (ip.row) {
        case RowTheme:
            c = [self cellWithStyle:UITableViewCellStyleValue1];
            c.textLabel.text = @"配色主题";
            c.detailTextLabel.text = [NSString stringWithFormat:@"%@", st.themeName];
            c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            break;
        case RowFont:
            c = [self cellWithStyle:UITableViewCellStyleValue1];
            c.textLabel.text = @"字体";
            c.detailTextLabel.text = st.fontName;
            c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            break;
        case RowFontSize:
            c = [self cellWithStyle:UITableViewCellStyleValue1];
            c.textLabel.text = @"字号";
            c.detailTextLabel.text = [NSString stringWithFormat:@"%.0f pt", st.fontSize];
            c.accessoryView = [self stepperMin:8 max:32 step:1 value:st.fontSize action:@selector(fontSizeChanged:)];
            break;
        case RowCursorStyle:
            c = [self cellWithStyle:UITableViewCellStyleValue1];
            c.textLabel.text = @"光标形状";
            c.detailTextLabel.text = CursorStyleName(st.cursorStyle);
            c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            break;
        case RowCursorBlink:
            c = [self cellWithStyle:UITableViewCellStyleDefault];
            c.textLabel.text = @"光标闪烁";
            c.accessoryView = [self switchWith:st.cursorBlink action:@selector(cursorBlinkChanged:)];
            break;
        default:
            c = [self cellWithStyle:UITableViewCellStyleDefault];
            c.textLabel.text = @"粗体当亮色";
            c.accessoryView = [self switchWith:st.boldAsBright action:@selector(boldBrightChanged:)];
            break;
        }
    } else if (ip.section == SecTerm) {
        switch (ip.row) {
        case RowScrollback:
            c = [self cellWithStyle:UITableViewCellStyleValue1];
            c.textLabel.text = @"回滚行数";
            c.detailTextLabel.text = [NSString stringWithFormat:@"%ld", (long)st.scrollbackLines];
            c.accessoryView = [self stepperMin:0 max:50000 step:1000 value:st.scrollbackLines action:@selector(scrollbackChanged:)];
            break;
        case RowBracket:
            c = [self cellWithStyle:UITableViewCellStyleDefault];
            c.textLabel.text = @"括号粘贴";
            c.accessoryView = [self switchWith:st.bracketPaste action:@selector(bracketChanged:)];
            break;
        case RowConfirmPaste:
            c = [self cellWithStyle:UITableViewCellStyleDefault];
            c.textLabel.text = @"多行粘贴前确认";
            c.accessoryView = [self switchWith:st.confirmMultilinePaste action:@selector(confirmPasteChanged:)];
            break;
        case RowBellHaptic:
            c = [self cellWithStyle:UITableViewCellStyleDefault];
            c.textLabel.text = @"响铃震动";
            c.accessoryView = [self switchWith:st.bellHaptic action:@selector(bellHapticChanged:)];
            break;
        case RowBellSound:
            c = [self cellWithStyle:UITableViewCellStyleDefault];
            c.textLabel.text = @"响铃声音";
            c.accessoryView = [self switchWith:st.bellSound action:@selector(bellSoundChanged:)];
            break;
        default:
            c = [self cellWithStyle:UITableViewCellStyleDefault];
            c.textLabel.text = @"纯 ASCII 键盘";
            c.accessoryView = [self switchWith:st.asciiKeyboard action:@selector(asciiKbChanged:)];
            break;
        }
    } else if (ip.section == SecStart) {
        c = [self cellWithStyle:UITableViewCellStyleValue1];
        if (ip.row == RowShell) {
            c.textLabel.text = @"Shell 路径";
            c.detailTextLabel.text = st.shellPath.length ? st.shellPath : @"自动";
        } else {
            c.textLabel.text = @"启动命令";
            c.detailTextLabel.text = st.startupCommand.length ? st.startupCommand : @"无";
        }
        c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    } else {
        c = [self cellWithStyle:UITableViewCellStyleValue1];
        if (ip.row == RowVersion) {
            c.textLabel.text = @"Terminal";
            c.detailTextLabel.text = @"1.0.0";
        } else {
            c.textLabel.text = @"中文输入";
            c.detailTextLabel.text = @"系统拼音键盘";
            c.textLabel.font = [UIFont systemFontOfSize:15];
            c.detailTextLabel.font = [UIFont systemFontOfSize:13];
        }
        c.selectionStyle = UITableViewCellSelectionStyleNone;
    }
    if (!c.accessoryView) c.textLabel.font = [UIFont systemFontOfSize:15];
    return c;
}

- (UISwitch *)switchWith:(BOOL)on action:(SEL)a
{
    UISwitch *s = [[UISwitch alloc] init];
    s.on = on;
    s.onTintColor = [UIColor colorWithRed:0.26 green:0.52 blue:0.92 alpha:1.0];
    [s addTarget:self action:a forControlEvents:UIControlEventValueChanged];
    return s;
}

/* ---------------- 开关 ---------------- */

- (void)cursorBlinkChanged:(UISwitch *)s { [TermSettings shared].cursorBlink = s.on; [self changed]; }
- (void)boldBrightChanged:(UISwitch *)s { [TermSettings shared].boldAsBright = s.on; [self changed]; }
- (void)bracketChanged:(UISwitch *)s { [TermSettings shared].bracketPaste = s.on; [self changed]; }
- (void)confirmPasteChanged:(UISwitch *)s { [TermSettings shared].confirmMultilinePaste = s.on; [self changed]; }
- (void)bellHapticChanged:(UISwitch *)s { [TermSettings shared].bellHaptic = s.on; [self changed]; }
- (void)bellSoundChanged:(UISwitch *)s { [TermSettings shared].bellSound = s.on; [self changed]; }
- (void)asciiKbChanged:(UISwitch *)s { [TermSettings shared].asciiKeyboard = s.on; [self changed]; }

- (void)fontSizeChanged:(UIStepper *)s
{
    [TermSettings shared].fontSize = (CGFloat)s.value;
    [self changed];
}

- (void)scrollbackChanged:(UIStepper *)s
{
    [TermSettings shared].scrollbackLines = (NSInteger)s.value;
    [self changed];
}

/* ---------------- 选择 ---------------- */

- (void)sheetTitle:(NSString *)title options:(NSArray *)options current:(NSInteger)current pick:(void (^)(NSInteger))pick
{
    UIAlertController *a = [UIAlertController alertControllerWithTitle:title
                                                              message:nil
                                                       preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSInteger i = 0; i < (NSInteger)options.count; i++) {
        NSString *name = options[i];
        UIAlertActionStyle style = (i == current) ? UIAlertActionStyleDefault : UIAlertActionStyleDefault;
        [a addAction:[UIAlertAction actionWithTitle:(i == current ? [name stringByAppendingString:@"  ✓"] : name)
                                             style:style
                                           handler:^(UIAlertAction *act) { pick(i); }]];
    }
    [a addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    a.popoverPresentationController.sourceView = self.view;
    a.popoverPresentationController.sourceRect = CGRectMake(self.view.bounds.size.width / 2, self.view.bounds.size.height / 2, 1, 1);
    [self presentViewController:a animated:YES completion:nil];
}

- (void)pickTheme
{
    NSMutableArray *names = [NSMutableArray array];
    for (int i = 0; TermThemes()[i].name; i++) {
        NSString *n = [NSString stringWithUTF8String:TermThemes()[i].displayName];
        [names addObject:[NSString stringWithFormat:@"%@  (%s)", n, TermThemes()[i].name]];
    }
    NSInteger cur = 0;
    for (int i = 0; TermThemes()[i].name; i++)
        if (strcmp(TermThemes()[i].name, [[TermSettings shared].themeName UTF8String]) == 0) cur = i;
    [self sheetTitle:@"配色主题" options:names current:cur pick:^(NSInteger i) {
        [TermSettings shared].themeName = [NSString stringWithUTF8String:TermThemes()[i].name];
        [self changed];
    }];
}

- (void)pickFont
{
    NSArray *fonts = FontChoices();
    NSInteger cur = [fonts indexOfObject:[TermSettings shared].fontName];
    [self sheetTitle:@"字体" options:fonts current:cur pick:^(NSInteger i) {
        [TermSettings shared].fontName = fonts[i];
        [self changed];
    }];
}

- (void)pickCursorStyle
{
    NSMutableArray *names = [NSMutableArray array];
    for (NSInteger i = 0; i <= 6; i++) [names addObject:CursorStyleName(i)];
    NSInteger cur = [TermSettings shared].cursorStyle;
    [self sheetTitle:@"光标形状" options:names current:cur pick:^(NSInteger i) {
        [TermSettings shared].cursorStyle = i;
        [self changed];
    }];
}

- (void)promptText:(NSString *)title current:(NSString *)current placeholder:(NSString *)ph apply:(void (^)(NSString *))apply
{
    UIAlertController *a = [UIAlertController alertControllerWithTitle:title
                                                              message:nil
                                                       preferredStyle:UIAlertControllerStyleAlert];
    [a addTextFieldWithConfigurationHandler:^(UITextField *tf) {
        tf.text = current ?: @"";
        tf.placeholder = ph;
        tf.autocapitalizationType = UITextAutocapitalizationTypeNone;
        tf.autocorrectionType = UITextAutocorrectionTypeNo;
        tf.keyboardType = UIKeyboardTypeASCIICapable;
    }];
    [a addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [a addAction:[UIAlertAction actionWithTitle:@"保存" style:UIAlertActionStyleDefault handler:^(UIAlertAction *act) {
        apply(a.textFields.firstObject.text ?: @"");
    }]];
    [self presentViewController:a animated:YES completion:nil];
}

- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip
{
    [tv deselectRowAtIndexPath:ip animated:YES];
    if (ip.section == SecLook) {
        if (ip.row == RowTheme) [self pickTheme];
        else if (ip.row == RowFont) [self pickFont];
        else if (ip.row == RowCursorStyle) [self pickCursorStyle];
    } else if (ip.section == SecStart) {
        TermSettings *st = [TermSettings shared];
        if (ip.row == RowShell) {
            [self promptText:@"Shell 路径" current:st.shellPath placeholder:@"留空自动探测" apply:^(NSString *v) {
                st.shellPath = v;
                [self changed];
            }];
        } else {
            [self promptText:@"启动命令" current:st.startupCommand placeholder:@"比如 cd ~ ;  echo 你好" apply:^(NSString *v) {
                st.startupCommand = v;
                [self changed];
            }];
        }
    }
}

@end
