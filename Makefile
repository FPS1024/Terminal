# Terminal - 越狱 iOS 终端
#
#   make test        主机端核心单测(macOS 上直接跑，不碰模拟器)
#   make dump        主机端联调工具，真起 zsh 看渲染结果
#   make ios         编译 iOS App(arm64，最低 iOS 12，结尾自动 ldid 伪签名)
#   make deb         打包成传统越狱的 deb
#   make deb-rootless  打包成无根越狱(/var/jb)的 deb
#   make install     推送到真机安装(需要 DEVICE=root@ip)
#   make appicon     从 macOS 自带 Terminal.app 里重新导出图标源图
#   make storyboard  重新用 ibtool 编译 LaunchScreen(需要 iOS 平台组件)
#   make clean

CC        ?= clang
MIN_IOS   ?= 12.0
ARCHS     ?= arm64
SDK       ?= iphoneos
DEVICE    ?= root@iphone.local

APP_NAME   = Terminal
BUNDLE_ID  = com.malacaihongpi.terminal
VERSION    = 1.0.0
BUILD      = build
APP_DIR    = $(BUILD)/$(APP_NAME).app
IOS_SDK    = $(shell xcrun --sdk $(SDK) --show-sdk-path)
INCS       = -Isrc/core -Isrc/host -Isrc/ios
CFLAGS     = -std=c99 -O2 -Wall -Wno-unused-parameter $(INCS)
FW         = -framework Foundation -framework UIKit -framework CoreGraphics \
             -framework CoreText -framework QuartzCore -framework AudioToolbox

CORE_SRC   = src/core/vt.c src/core/vt_unicode.c
HOST_SRC   = src/host/pty.c src/host/platform.c
IOS_SRC    = $(wildcard src/ios/*.m) $(CORE_SRC) $(HOST_SRC)
IOS_OBJ    = $(patsubst %.m,$(BUILD)/%.o,$(patsubst %.c,$(BUILD)/%.o,$(IOS_SRC)))

ICON_SIZES  = Icon.png:60 Icon@2x.png:120 Icon@3x.png:180 \
              Icon-76.png:76 Icon-76@2x.png:152 Icon-83.5@2x.png:167
APPICON_SRC = src/ios/AppIconSource.png

.PHONY: all test dump ios deb deb-rootless install icons appicon storyboard clean
all: test ios

# ---------------- 主机端(可以在这台 Mac 上直接验证) ----------------

test: $(BUILD)/test_vt
	@$(BUILD)/test_vt

$(BUILD)/test_vt: tests/test_vt.c $(CORE_SRC)
	@mkdir -p $(BUILD)
	$(CC) -std=c99 -g -O1 -Wall -Wno-unused-parameter -Isrc/core -o $@ $^

dump: $(BUILD)/termdump

$(BUILD)/termdump: host/termdump.c $(CORE_SRC) $(HOST_SRC)
	@mkdir -p $(BUILD)
	$(CC) -std=c99 -g -O1 -Wall -Isrc/core -o $@ $^

# ---------------- 图标 ----------------

icons: $(BUILD)/mkicon
	@mkdir -p $(BUILD)/icons
	@for spec in $(ICON_SIZES); do \
		name=$${spec%%:*}; size=$${spec##*:}; \
		if [ -f $(APPICON_SRC) ]; then \
			$(BUILD)/mkicon $(BUILD)/icons/$$name $$size $(APPICON_SRC); \
		else \
			$(BUILD)/mkicon $(BUILD)/icons/$$name $$size; \
		fi; \
	done
	@echo "ICONS -> $(BUILD)/icons"

# 图标源图来自 macOS 自带的 Terminal.app（Apple 的美术资源，只在本机自己用）
appicon:
	@mkdir -p $(BUILD)
	rm -rf $(BUILD)/Terminal.iconset
	iconutil -c iconset /System/Applications/Utilities/Terminal.app/Contents/Resources/Terminal.icns \
		-o $(BUILD)/Terminal.iconset
	@cp $(BUILD)/Terminal.iconset/icon_128x128@2x.png $(APPICON_SRC)
	@echo "APPICON -> $(APPICON_SRC)"

$(BUILD)/mkicon: tools/mkicon.c
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -o $@ $< -framework CoreGraphics -framework ImageIO -framework CoreFoundation

# ---------------- iOS ----------------

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -arch $(ARCHS) -miphoneos-version-min=$(MIN_IOS) -isysroot $(IOS_SDK) -c $< -o $@

$(BUILD)/%.o: %.m
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fobjc-arc -arch $(ARCHS) -miphoneos-version-min=$(MIN_IOS) -isysroot $(IOS_SDK) -c $< -o $@

ios: icons $(APP_DIR)/$(APP_NAME)

$(APP_DIR)/$(APP_NAME): $(IOS_OBJ) src/ios/Info.plist
	@mkdir -p $(APP_DIR)
	$(CC) -arch $(ARCHS) -miphoneos-version-min=$(MIN_IOS) -isysroot $(IOS_SDK) $(FW) -o $@ $(IOS_OBJ)
	@cp src/ios/Info.plist $(APP_DIR)/Info.plist
	@rm -rf $(APP_DIR)/LaunchScreen.storyboardc
	@if [ -d src/ios/LaunchScreen.storyboardc ]; then \
		cp -R src/ios/LaunchScreen.storyboardc $(APP_DIR)/LaunchScreen.storyboardc; \
	fi
	@for f in $(BUILD)/icons/*.png; do \
		[ -e "$$f" ] || continue; \
		cp "$$f" "$(APP_DIR)/$$(basename $$f)"; \
	done
	@if command -v ldid >/dev/null 2>&1; then ldid -S $@ && echo "SIGNED -> $@"; else echo "warn: 没找到 ldid，未签名"; fi
	@echo "APP -> $(APP_DIR)"

storyboard:
	@rm -rf src/ios/LaunchScreen.storyboardc
	xcrun ibtool --compile src/ios/LaunchScreen.storyboardc \
		--target-device iphone --target-device ipad \
		--minimum-deployment-target $(MIN_IOS) src/ios/LaunchScreen.storyboard
	@echo "STORYBOARD -> src/ios/LaunchScreen.storyboardc"

# ---------------- 打包 ----------------

deb: ios
	APP_NAME=$(APP_NAME) BUNDLE_ID=$(BUNDLE_ID) VERSION=$(VERSION) ROOTLESS=0 sh packaging/mkdeb.sh

deb-rootless: ios
	APP_NAME=$(APP_NAME) BUNDLE_ID=$(BUNDLE_ID) VERSION=$(VERSION) ROOTLESS=1 sh packaging/mkdeb.sh

install: ios
	@tar -czf $(BUILD)/$(APP_NAME).tar.gz -C $(BUILD) $(APP_NAME).app
	scp $(BUILD)/$(APP_NAME).tar.gz $(DEVICE):/tmp/
	ssh $(DEVICE) 'set -e; P=/Applications; [ -d /var/jb ] && P=/var/jb/Applications; \
		rm -rf $$P/$(APP_NAME).app; tar -xzf /tmp/$(APP_NAME).tar.gz -C $$P; \
		uicache -a >/dev/null 2>&1 || uicache -p $$P/$(APP_NAME).app >/dev/null 2>&1 || true; \
		echo installed to $$P/$(APP_NAME).app'

clean:
	rm -rf $(BUILD) dist
