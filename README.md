# Terminal

面向越狱 iOS 设备的终端应用。以原生 App 形式运行，带主屏幕图标，最低支持 **iOS 12.0**。

---

## 一、项目简介

Terminal 是一个为越狱 iOS 设备实现的终端模拟器与 Shell 宿主，面向个人设备上的日常
命令行使用。项目的设计重点集中在两个移动端长期被忽视的问题上：

1. **中文可用性。** 终端内的中文输入依赖输入法的预编辑（preedit）与候选条，这与传统
   按键驱动的终端模型并不兼容：如果直接把输入法的中间态送进 PTY，命令行会被拼音字母
   污染。本项目通过完整实现 `UITextInput` 协议解决该问题，预编辑内容只参与渲染，
   仅上屏文字才会写入 PTY。
2. **低系统版本兼容。** 越狱社区中相当数量的设备仍停留在 iOS 12–14。因此本项目不使用
   SwiftUI，也不引入 Swift 运行时，界面层完全基于 Objective-C 与 UIKit 实现。

此外，终端仿真核心与平台相关代码以纯 C 编写，不依赖任何 UI 框架，因此可以在 macOS
上直接进行单元测试与人工验证，无需模拟器或真机。

---

## 二、功能特性

### 2.1 终端仿真

- 兼容 VT100 / VT220，并实现主要 xterm 扩展：CSI / DCS / OSC 解析、DEC 特殊图形
  字符集、滚动区域、字符插入与删除、备用屏幕（47 / 1047 / 1048 / 1049）
- 模式支持：自动换行、原点模式、插入模式、应用光标键、应用小键盘、反向前折行、
  置灰输出、括号粘贴（2004）、同步输出（2026）、焦点事件（1004）
- SGR 属性：粗体、暗色、斜体、下划线、双下划线、删除线、闪烁、反显、隐藏；
  支持 256 色与 24 位真彩色（分号与冒号两种参数写法均可解析）
- 每个单元格可保存最多三个码点，组合字符、变体选择符与 ZWJ 序列均可正确存储与渲染
- 内置东亚字符宽度表（全角、零宽、emoji），中英文混排严格按列对齐
- 滚动区采用环形缓冲，屏幕滚动通过行指针移动实现，高吞吐输出下不产生明显卡顿
- 窗口尺寸变化时按逻辑行重新折行（reflow），横竖屏切换与字号调整不会破坏历史内容
- OSC 支持：0/2 窗口标题、4 调色板重定义、52 剪贴板、10/11/12 前景/背景/光标颜色、
  133 Shell 集成标记
- 自动应答终端查询：CPR、DA、DECRQM、DECRQSS 与 termcap 查询，可正常承载
  tmux、vim、htop 等全屏程序
- 鼠标上报：X10、1000、1002、1003 模式，支持 SGR 编码

### 2.2 中文支持

- **输入**：使用系统输入法，拼音候选条正常显示。预编辑内容绘制于光标位置并带下划线，
  **不会**写入 PTY；仅候选词上屏后才会发送至 Shell。
- **显示**：CoreText 级联字体链
  `Menlo → PingFang SC → Hiragino Sans GB → Heiti SC → Apple Color Emoji`，
  并通过字距校正将每段文本对齐到整数个单元格宽度。
- **粘贴**：默认启用括号粘贴；远端未启用时自动将换行转换为回车；多行内容粘贴前
  弹出确认对话框。
- **选择**：双击选词、三击选整行、长按选词后拖动扩选；中文按连续汉字串视为一个词。

### 2.3 应用层

- 多会话标签页，标题跟随 OSC 0/2 自动更新
- 键盘上方快捷键条（可横向滚动）：`esc`、`tab`、`⇧tab`、`ctrl`、`alt`、`^C`、`^D`、
  `^Z`、方向键、`home`、`end`、`pgup`、`pgdn`、`del`、常用符号、粘贴、复制、清屏
- `ctrl` / `alt` 为一次性修饰键：按下后作用于下一个按键，随后自动复位
- 硬件键盘支持（通过 `pressesBegan` 处理），方向键、`esc`、`tab`、`Ctrl` 组合键均可用；
  方向键序列会根据 DECCKM 模式自动切换
- 触摸手势：单指拖动滚动、双指捏合调整字号、单击聚焦、双击选词、三击选行、长按选词
- 五套内置配色：`dark`、`light`、`solarized-dark`、`dracula`、`classic`
- 设置界面：主题、字体、字号、光标形状、光标闪烁、回滚行数、括号粘贴、粘贴确认、
  提示音震动与声音、ASCII 键盘、Shell 路径、启动命令
- 支持通过 `~/.terminalrc` 进行配置（见第五节）
- 自动探测无根越狱环境：存在 `/var/jb` 时优先使用该前缀下的路径与 Shell

---

## 三、技术选型说明

项目的分层与语言选择如下：

| 层次 | 语言 | 位置 | 说明 |
| --- | --- | --- | --- |
| 终端仿真核心 | C99 | `src/core/` | 无 UI 依赖，可在 macOS 上直接单测 |
| 平台 / PTY 层 | C99 | `src/host/` | forkpty、环境变量、越狱路径探测，iOS 与 macOS 共用 |
| 渲染与输入 | Objective-C / UIKit | `src/ios/TerminalView.*` | 实现 `UITextInput` 与 CoreText 渲染 |
| 应用层 | Objective-C / UIKit | `src/ios/` | 控制器、设置、会话管理 |

不使用 SwiftUI 的原因：

- SwiftUI 要求 iOS 13 及以上，`@main` 与 Scene 生命周期同样要求 iOS 13；本项目需要覆盖
  iOS 12。
- Swift ABI 稳定性自 iOS 12.2 起才得到保证，在低版本设备上引入 Swift 运行时会增大
  包体积并提高崩溃风险。
- **关键原因**：中文输入法的候选条是 `UITextInput` 协议层面的能力，SwiftUI 的
  `TextField` 不提供相应的控制点，无法满足本项目对预编辑文本的处理要求。

终端仿真核心之所以采用 C 而非 Objective-C，是为了让它完全脱离 UI 框架，从而能够在
构建机上以单元测试的方式验证——宽字符列宽、折行、reflow 与 SGR 状态机等逻辑仅靠
真机观察难以覆盖。

---

## 四、构建与安装

### 4.1 环境要求

- macOS 与 Xcode（仅需 iPhoneOS SDK，**不需要模拟器**，本项目也不使用模拟器）
- 可选：`ldid`（用于伪签名），未安装时构建仍可完成，但产物为未签名状态

### 4.2 构建命令

```sh
make test          # 主机端核心单元测试
make dump          # 主机端联调工具：启动真实 Shell 并输出最终屏幕内容
make ios           # 编译 arm64 App（最低 iOS 12），结束时自动执行 ldid 伪签名
make deb           # 打包为传统越狱 deb（安装至 /Applications）
make deb-rootless  # 打包为无根越狱 deb（安装至 /var/jb/Applications）
```

构建产物位于 `build/Terminal.app`，安装包位于 `dist/`。

在不使用模拟器与真机的前提下验证渲染效果：

```sh
./build/termdump -c 48 -r 10 -n 2 -- '/bin/zsh /tmp/demo.sh'
```

### 4.3 安装方式

**方式一：deb 包**

```sh
make deb-rootless        # 依据越狱类型选择对应目标
# 将 dist/Terminal_1.0.0_iphoneos-arm64.deb 传输至设备后执行：
dpkg -i Terminal_1.0.0_iphoneos-arm64.deb
```

`postinst` 脚本会自动执行 `uicache` 并刷新 SpringBoard。

**方式二：直接推送**

```sh
make install DEVICE=root@192.168.1.23
```

该目标会完成打包、传输、解包至 `/Applications`（或 `/var/jb/Applications`）并执行
`uicache`。

> 若未安装 `ldid`，产物为未签名状态。已安装 AppSync 或关闭了签名校验的设备可直接运行；
> 否则请将 `ldid` 加入 `PATH` 后重新执行 `make ios`。

---

## 五、使用说明

### 5.1 中文输入

1. 确认系统已启用中文键盘（设置 → 通用 → 键盘 → 键盘 → 添加「简体中文 - 拼音」）。
2. 在终端中点击以唤起键盘，切换至拼音键盘后正常输入，候选条将显示在键盘上方。
3. 候选条中尚未上屏的字母属于预编辑内容，绘制于光标位置并带下划线，不会被发送至
   Shell。按下空格、回车或选择候选词后，文本才会写入 PTY。
4. 如需在终端内快速切换中英文，可使用系统键盘的地球键。也可在设置中启用
   「纯 ASCII 键盘」，此时默认使用英文布局（拼音输入法基于 ASCII 键盘，仍可正常使用）。

若中文显示为方块或列宽异常，通常是字体名称不正确，可在设置中切换为 `PingFang SC`
或 `Hiragino Sans GB`。

### 5.2 手势与快捷键

| 操作 | 效果 |
| --- | --- |
| 单击 | 聚焦；存在选区时取消选区；鼠标模式下发送鼠标点击 |
| 双击 | 选中一个词 |
| 三击 | 选中整行 |
| 长按 | 选中一个词并可拖动扩选；在空白处长按则弹出粘贴菜单 |
| 单指拖动 | 滚动回滚区 |
| 双指捏合 | 调整字号（结果会写入设置） |
| 快捷键条 `ctrl` 后按 `c` | 等价于 `Ctrl-C` |

硬件键盘可直接使用（需 Lightning/USB 转接器或蓝牙连接）。方向键、`esc`、`tab`、
`Ctrl-<字母>` 与 `Alt-<字符>` 均受支持。

### 5.3 配置文件 `~/.terminalrc`

配置文件位于运行 App 的用户主目录下（越狱设备通常为 `/var/mobile/.terminalrc`）。
格式为 `key=value`，`#` 之后为注释。修改后需重启 App 生效。

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

需要注意：配置文件的值会覆盖设置界面中保存的值，且每次启动都会重新读取。

---

## 六、项目结构

```
src/core/vt.h  vt.c                  终端仿真核心（纯 C，可单元测试）
src/core/vt_unicode.h  .c            UTF-8 编解码与东亚字符宽度表
src/host/pty.h  pty.c                forkpty 会话封装
src/host/platform.h  .c              越狱路径探测、Shell 选择、环境变量组装
src/ios/TerminalView.h  .m           UITextInput 实现与 CoreText 渲染
src/ios/TerminalSession.h  .m        Vt 与 PTY 的粘合层，含后台读取线程
src/ios/TerminalViewController.h .m  多会话管理、快捷键条、手势接线
src/ios/Settings.h  .m               主题定义与配置读写
src/ios/SettingsViewController.h .m  设置界面
host/termdump.c                      构建机上的联调工具
tools/mkicon.c                       App 图标生成
packaging/mkdeb.sh                   在 macOS 上构造 deb（不依赖 dpkg）
```

---

## 七、测试

终端仿真核心附带 164 项断言，覆盖 UTF-8 解码、东亚字符宽度、折行与 reflow、滚动区、
备用屏幕、SGR、键序列编码、括号粘贴与跨块 UTF-8 序列等场景，测试语料以中文为主。

```sh
make test
```

---

## 八、已知限制

- `src/ios/LaunchScreen.storyboardc` 为预编译产物并一并纳入版本控制，原因是 `ibtool`
  在部分环境下需要访问 iOS 平台组件。需要修改启动画面时，编辑 `.storyboard` 后执行
  `make storyboard`。
- 未实现六键等少数键盘协议，也不提供软键盘的六键映射。
- 光标移动不会反向驱动 Shell 光标。系统的光标移动请求（例如键盘双指拖动）无法与
  系统自身的状态校正可靠区分，误发将导致方向键被插入命令行，因此本项目选择忽略此类
  请求。移动光标请使用快捷键条上的方向键或硬件键盘。
- 暂未实现分屏、文本搜索与链接识别。

---

## 九、图标说明

App 图标提取自 macOS 内置的 `Terminal.app`（`Contents/Resources/Terminal.icns`），
通过 `make appicon` 可重新导出，`make icons` 会将其裁切并缩放为 iOS 所需的各尺寸。

**该图标为 Apple 的美术资源，不属于本项目代码，亦不适用本项目的开源许可。**
如需在公开分发场景中使用，请自行替换为拥有合法授权的图标；移除
`src/ios/AppIconSource.png` 后，构建会自动回退到 `tools/mkicon.c` 内置生成的图标。

---

## 十、许可证

本项目源代码以 **MIT License** 发布，完整条款见 [LICENSE](LICENSE)。

Copyright (c) 2026 Kaysarjan Kasim <ceaser.k.w@outlook.com>

如前所述，App 图标来源于 Apple 的 `Terminal.app`，不在该许可的授权范围内。
