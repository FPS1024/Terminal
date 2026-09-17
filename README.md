# Terminal

越狱 iOS 设备上的终端 App。带桌面图标，不是命令行工具。

专门为**低版本系统**做：**最低 iOS 12.0**，rootful / rootless 越狱都能装。
中文输入、中文显示、中文粘贴都是按"能日常用"的标准做的，不是"能显示个汉字"的程度。

---

## 为什么是 C 引擎 + Objective-C 界面

这是这个项目最容易被问的问题，先讲清楚：

- **`src/core/` 是纯 C 的终端模拟器**（VT100/VT220 + xterm 扩展）。它不依赖任何 UI，
  所以能在 Mac 上 `make test` 直接跑 164 条断言、`make dump` 起一个真 zsh 看渲染结果。
  终端模拟里最容易错的就是这块（宽字符占几列、折行、reflow、字符集、SGR 状态机），
  这部分**必须可测**，不能靠"上真机看看"。
- **界面是 Objective-C + UIKit，不是 SwiftUI**：
  - SwiftUI 要 iOS 13+，`@main`/Scene 要 iOS 13+；越狱机大量停在 iOS 12/13/14。
  - Swift 的 ABI 稳定是 iOS 12.2 才有的，带 Swift 运行时的越狱 App 在低版本上更容易崩，
    包也大一圈。
  - 最关键：**中文输入法的候选条是 `UITextInput` 协议的事**，
    SwiftUI 的 `TextField` 根本不给你这个控制权。
- **`src/host/` 也是纯 C**（forkpty / 环境变量 / 越狱路径探测），iOS 和 macOS 两边共用。

所以不是"用 C 写 App"，是"终端引擎用 C，App 用 UIKit"。

---

## 功能

**终端核心**
- VT100/VT220 + xterm 扩展：CSI/DCS/OSC、DEC 特殊图形字符集、滚动区、插入/删除、
  备用屏幕（47/1047/1048/1049）、括号粘贴（2004）、同步输出（2026）、焦点事件（1004）
- SGR：粗体/暗色/斜体/下划线/双下划线/删除线/闪烁/反显/隐藏、256 色、真彩色
  （分号和冒号两种参数写法都吃）
- 每个格子能存 3 个码点，所以 `组合字符`、ZWJ emoji 序列不会散架
- 东亚字符宽度表（全角/零宽/emoji），中文、emoji 排版严丝合缝
- 滚屏用行指针搬家，回滚区是环形缓冲，`yes` 刷屏不卡
- 改窗口大小按逻辑行 **reflow**，横竖屏切换、捏合改字号，历史都不会乱成一团
- OSC 0/2 标题、OSC 4 改调色板、OSC 52 剪贴板、OSC 10/11/12 前景背景光标色、
  OSC 133 shell 集成标记
- 自动响应 CPR / DA / DECRQM / DECRQSS / termcap 查询（tmux、vim、htop 都能正常跑）
- 鼠标上报（X10 / 1000 / 1002 / 1003，SGR 编码）

**中文相关（重点）**
- 输入：用系统键盘，拼音候选条正常弹；**预编辑（marked text）只画在光标处，不会漏进 shell**，
  只有真正上屏的字才发给 pty
- 显示：CoreText 级联字体 `Menlo → PingFang SC → Hiragino Sans GB → Heiti SC → Apple Color Emoji`，
  中文前的英文按同一套格子宽算，不会一半对齐一半不对齐
- 粘贴：默认开启括号粘贴；远端没开时自动把换行转成回车；多行内容粘贴前弹确认框
- 复制：双击选词、三击选整行、长按选词并拖拽扩选（中文按"一整串汉字"算一个词）

**App 层**
- 多会话标签（顶部标签条，标题跟 OSC 0/2 走）
- 快捷键条（键盘上方，可横向滑）：`esc` `tab` `⇧tab` `ctrl` `alt` `^C` `^D` `^Z`
  `←` `↑` `↓` `→` `⇱` `⇲` `pgup` `pgdn` `del` 常用符号 `粘贴` `复制` `清屏`
- `ctrl` / `alt` 是**一次性**的：按一下，作用于下一个键，然后自动弹起
- 硬件键盘支持（`pressesBegan`），方向键、esc、tab、ctrl 组合都能用
- 手势：拖动滚动、捏合改字号、单击聚焦/清选区、双击选词、三击选行、长按选词
- 5 套配色：`dark` / `light` / `solarized-dark` / `dracula` / `classic`
- 设置页：主题、字体、字号、光标形状、光标闪烁、回滚行数、括号粘贴、粘贴确认、
  响铃震动/声音、ASCII 键盘、Shell 路径、启动命令
- `~/.terminalrc` 也能配（见下）
- 无根越狱自动探测：有 `/var/jb` 就用它下面的 shell 和路径

---

## 构建

需要 macOS + Xcode（只要 iPhoneOS SDK，**不需要模拟器**，本项目也不跑模拟器）。

```sh
make test          # 主机端核心单测，164 项断言
make dump          # 主机端联调：真起一个 zsh，把屏幕 dump 出来看
make ios           # 编译 arm64 App（最低 iOS 12），结尾自动 ldid 伪签名
make deb           # 传统越狱 deb（装到 /Applications）
make deb-rootless  # 无根越狱 deb（装到 /var/jb/Applications）
```

`make ios` 产物在 `build/Terminal.app`，`make deb` 产物在 `dist/`。

想确认渲染效果，不用真机也不用模拟器：

```sh
./build/termdump -c 48 -r 10 -n 2 -- '/bin/zsh /tmp/cn.sh'
```

---

## 安装

**方式一：deb**

```sh
make deb-rootless        # 视你的越狱类型选一个
# 把 dist/Terminal_1.0.0_iphoneos-arm64.deb 传到手机上，然后
dpkg -i Terminal_1.0.0_iphoneos-arm64.deb
```

`postinst` 会自动跑 `uicache` 并刷新 SpringBoard。

**方式二：直接推**

```sh
make install DEVICE=root@192.168.1.23
```

会打包、scp、解到 `/Applications`（或 `/var/jb/Applications`）、跑 `uicache`。

> 没有 `ldid` 的话 App 是未签名的。装了 AppSync 或者越狱本身关了签名校验的机器能直接跑，
> 否则请把 `ldid` 放到 PATH 再重新 `make ios`。

---

## 怎么用中文

1. 系统里要有中文键盘（设置 → 通用 → 键盘 → 键盘 → 添加"简体中文 - 拼音"）。
2. 终端里点一下唤起键盘，切到拼音键盘，正常打字，候选条会弹在键盘上方。
3. 候选条里那些还没上屏的字母是"预编辑"，画在光标位置带下划线，**不会**发给 shell。
   `空格`/`回车`/点候选词之后才真正发给 pty。
4. 想在终端里立刻切中英，用系统键盘的地球键；或者设置里打开「纯 ASCII 键盘」，
   那样默认就是英文布局（此时中文输入法也能用，拼音本来就是 ASCII 键盘）。

如果中文没显示成方块或者宽度对不上，基本是字体名写错了 —— 在设置里换 `PingFang SC`
或 `Hiragino Sans GB` 试试。

---

## 手势和快捷键

| 操作 | 效果 |
| --- | --- |
| 单击 | 聚焦 / 取消选区 / 如果是鼠标模式就发鼠标点击 |
| 双击 | 选中一个词 |
| 三击 | 选中整行 |
| 长按 | 选中一个词，然后拖着扩选；空白处长按 = 弹粘贴菜单 |
| 单指拖动 | 滚动回滚区 |
| 双指捏合 | 改字号（会存到设置里） |
| 快捷键条 `ctrl` 再按 `c` | 等于 `Ctrl-C` |

硬件键盘直接插上就能用（需要 Lightning/USB 转接或蓝牙）。方向键、`esc`、`tab`、
`Ctrl-<字母>`、`Alt-<字符>` 都支持，方向键会按 DECCKM 模式自动切换 `ESC [ A` / `ESC O A`。

---

## `~/.terminalrc`

放在运行 App 那个用户的 home 目录（越狱机上一般就是 `/var/mobile/.terminalrc`）。
`key=value`，`#` 后面是注释。改完重启 App 生效。

```ini
theme           = dracula     # dark / light / solarized-dark / dracula / classic
font            = Menlo
font_size       = 15
scrollback      = 8000
shell           = /var/jb/bin/zsh
startup         = cd ~ ; ls
cursor          = 2           # 0=跟随程序 1=方块 2=竖线 3=下划线 4=闪烁下划线 5=闪烁竖线 6=闪烁方块
cursor_blink    = 1
bold_as_bright  = 1
bell_haptic     = 1
bell_sound      = 0
ascii_keyboard  = 0
confirm_paste   = 1
```

注意：配置文件会覆盖设置页里存的值（每次启动都会重读）。

---

## 目录结构

```
src/core/vt.h  vt.c          终端模拟核心（纯 C，可单测）
src/core/vt_unicode.h  .c    UTF-8 编解码 + 东亚宽度表
src/host/pty.h  pty.c        forkpty 会话封装
src/host/platform.h  .c      越狱路径探测 / shell 选择 / 环境变量
src/ios/TerminalView.h  .m   UITextInput + CoreText 渲染（中文输入法的关键在这）
src/ios/TerminalSession.h .m Vt 和 pty 的粘合层，带后台读线程
src/ios/TerminalViewController.h .m  多会话、快捷键条、手势接线
src/ios/Settings.h  .m       主题和配置
src/ios/SettingsViewController.h .m  设置页
host/termdump.c              Mac 上的联调工具
tools/mkicon.c               图标生成（纯 CoreGraphics，无美术资源）
packaging/mkdeb.sh           在 macOS 上拼 deb（不需要 dpkg）
```

---

## 已知限制

- `src/ios/LaunchScreen.storyboardc` 是**预编译好的**一起提交的，因为 `ibtool` 在这台机器上
  需要访问 iOS 平台组件（沙箱外）。要改启动图，编辑 `.storyboard` 后跑 `make storyboard`。
- 不支持六键 / 部分少见键盘协议；也不做软键盘的六键映射。
- 光标移动不会反过来去动 shell 光标（系统的"双指拖光标"请求会被忽略），
  因为没法可靠区分"用户真的想移动"和"系统自己在纠正"，发错了就是往命令行里插方向键。
  要移光标请用快捷键条上的方向键。
- 没有实现分屏、搜索、URL 嗅探。这些留着以后加。

---

## 许可

自己用，随便改。
