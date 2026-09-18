# Terminal — 越狱 iOS 终端
#
# 约定：源码在 src/ tests/ tools/ packaging/ assets/，
#       中间文件全部落在 build/，发布产物全部落在 out/（out/ 里只有最终产物）。
#
#   make ios        编译 App                 -> out/Terminal.app
#   make deb        打包 rootless 越狱 deb    -> out/Terminal_1.0.1_iphoneos-arm64.deb
#   make ipa        打包 ipa                 -> out/Terminal_1.0.1.ipa
#   make release    依次生成上面三个产物
#
#   make test       主机端核心单元测试（不需要模拟器，也不需要真机）
#   make dump       主机端联调工具：起真实 Shell，打印最终屏幕
#   make install    推送到真机安装（需要 DEVICE=root@ip）
#   make clean      清掉 build/ 与 out/
#
# 变量：VERSION / MIN_IOS / ARCHS / DEVICE 均可在命令行覆盖。

# ---------------- 项目信息 ----------------
APP_NAME   := Terminal
BUNDLE_ID  := com.malacaihongpi.terminal
VERSION    := 1.0.1
MAINTAINER := FPS1024 <ceaser.k.w@outlook.com>

# ---------------- 工具链 ----------------
CC       ?= clang
MIN_IOS  ?= 12.0
ARCHS    ?= arm64
SDK      ?= iphoneos
DEVICE   ?= root@iphone.local

IOS_SDK := $(shell xcrun --sdk $(SDK) --show-sdk-path 2>/dev/null)

# rootless 越狱（Dopamine / Palera1n 等）：安装前缀固定为 /var/jb
DEB_ARCH := iphoneos-arm64

# ---------------- 目录约定 ----------------
SRC_DIR    := src
TEST_DIR   := tests
TOOLS_DIR  := tools
PKG_DIR    := packaging
ASSET_DIR  := assets

BUILD_DIR  := build
OBJ_DIR    := $(BUILD_DIR)/obj
ICON_DIR   := $(BUILD_DIR)/icons
DEB_WORK   := $(BUILD_DIR)/deb
IPA_WORK   := $(BUILD_DIR)/ipa

OUT_DIR    := out
APP_DIR    := $(OUT_DIR)/$(APP_NAME).app
DEB_PKG    := $(OUT_DIR)/$(APP_NAME)_$(VERSION)_$(DEB_ARCH).deb
IPA_PKG    := $(OUT_DIR)/$(APP_NAME)_$(VERSION).ipa

# ---------------- 源文件 ----------------
CORE_SRC := $(SRC_DIR)/core/vt.c $(SRC_DIR)/core/vt_unicode.c
HOST_SRC := $(SRC_DIR)/host/pty.c $(SRC_DIR)/host/platform.c
IOS_SRC  := $(wildcard $(SRC_DIR)/ios/*.m) $(CORE_SRC) $(HOST_SRC)
IOS_OBJ  := $(patsubst %.m,$(OBJ_DIR)/%.o,$(patsubst %.c,$(OBJ_DIR)/%.o,$(IOS_SRC)))

IOS_INCS    := -I$(SRC_DIR)/core -I$(SRC_DIR)/host -I$(SRC_DIR)/ios
IOS_CFLAGS  := -std=c99 -O2 -Wall -Wno-unused-parameter $(IOS_INCS) \
               -arch $(ARCHS) -miphoneos-version-min=$(MIN_IOS) -isysroot $(IOS_SDK)
IOS_LDFLAGS := -arch $(ARCHS) -miphoneos-version-min=$(MIN_IOS) -isysroot $(IOS_SDK) \
               -framework Foundation -framework UIKit -framework CoreGraphics \
               -framework CoreText -framework QuartzCore -framework AudioToolbox

LAUNCHSCREEN := $(SRC_DIR)/ios/LaunchScreen.storyboardc

# ---------------- App 图标 ----------------
APPICON_SRC := $(ASSET_DIR)/AppIconSource.png
ICON_SPECS  := Icon.png:60 Icon@2x.png:120 Icon@3x.png:180 \
               Icon-76.png:76 Icon-76@2x.png:152 Icon-83.5@2x.png:167
ICON_FILES  := $(addprefix $(ICON_DIR)/,Icon.png Icon@2x.png Icon@3x.png \
               Icon-76.png Icon-76@2x.png Icon-83.5@2x.png)

.PHONY: help all ios deb ipa release test dump install icons appicon storyboard clean check-sdk

.DEFAULT_GOAL := help

help:
	@echo
	@echo "  Terminal — 越狱 iOS 终端"
	@echo
	@echo "  发布产物（统一输出到 $(OUT_DIR)/）"
	@printf '    %-14s %s\n' "make ios"     "编译 App        -> $(APP_DIR)"
	@printf '    %-14s %s\n' "make deb"     "rootless deb 包 -> $(DEB_PKG)"
	@printf '    %-14s %s\n' "make ipa"     "ipa 包          -> $(IPA_PKG)"
	@printf '    %-14s %s\n' "make release" "上面三个一起生成"
	@echo
	@echo "  主机端（不需要模拟器与真机）"
	@printf '    %-14s %s\n' "make test"    "终端核心单元测试"
	@printf '    %-14s %s\n' "make dump"    "起真实 Shell，打印最终屏幕"
	@echo
	@echo "  其他"
	@printf '    %-14s %s\n' "make install" "推送到真机安装（DEVICE=root@ip）"
	@printf '    %-14s %s\n' "make icons"   "重新生成 App 图标"
	@printf '    %-14s %s\n' "make clean"   "清掉 $(BUILD_DIR)/ 与 $(OUT_DIR)/"
	@echo

all: release

# ---------------- 发布产物 ----------------

ios: check-sdk icons $(APP_DIR)/$(APP_NAME)

deb: ios
	@APP_NAME='$(APP_NAME)' BUNDLE_ID='$(BUNDLE_ID)' VERSION='$(VERSION)' \
	 DEB_ARCH='$(DEB_ARCH)' MAINTAINER='$(MAINTAINER)' \
	 APP_DIR='$(APP_DIR)' OUT_DEB='$(DEB_PKG)' WORK='$(DEB_WORK)' \
	 sh $(PKG_DIR)/mkdeb.sh

ipa: ios
	@APP_NAME='$(APP_NAME)' VERSION='$(VERSION)' \
	 APP_DIR='$(APP_DIR)' OUT_IPA='$(IPA_PKG)' WORK='$(IPA_WORK)' \
	 sh $(PKG_DIR)/mkipa.sh

release: ios deb ipa
	@echo
	@echo "  发布产物（$(OUT_DIR)/）"
	@for f in '$(APP_DIR)' '$(DEB_PKG)' '$(IPA_PKG)'; do \
		printf '    %-46s %8s\n' "$$f" "$$(du -sh "$$f" | cut -f1)"; \
	done
	@echo
	@echo "  SHA256（用于 Release 校验）"
	@for f in '$(DEB_PKG)' '$(IPA_PKG)'; do \
		printf '    %-46s %s\n' "$$(basename "$$f")" "$$(LC_ALL=C shasum -a 256 "$$f" | cut -d' ' -f1)"; \
	done
	@echo

check-sdk:
	@test -n "$(IOS_SDK)" || { \
		echo "错误: 找不到 iPhoneOS SDK（xcrun --sdk $(SDK) --show-sdk-path 无输出）。" >&2; \
		echo "      请安装 Xcode，本项目不需要模拟器。" >&2; \
		exit 1; \
	}

$(APP_DIR)/$(APP_NAME): $(IOS_OBJ) $(SRC_DIR)/ios/Info.plist $(ICON_FILES)
	@mkdir -p $(dir $@)
	@rm -rf $(APP_DIR)
	@mkdir -p $(APP_DIR)
	@$(CC) $(IOS_LDFLAGS) -o $@ $(IOS_OBJ)
	@cp $(SRC_DIR)/ios/Info.plist $(APP_DIR)/Info.plist
	@cp $(ICON_FILES) $(APP_DIR)/
	@if [ -d $(LAUNCHSCREEN) ]; then cp -R $(LAUNCHSCREEN) $(APP_DIR)/; fi
	@find $(APP_DIR) -name '.DS_Store' -delete 2>/dev/null || true
	@if command -v ldid >/dev/null 2>&1; then \
		ldid -S $@ && echo "  SIGN  $@"; \
	else \
		echo "  WARN  未找到 ldid，产物未伪签名"; \
	fi
	@echo "  APP   $(APP_DIR)"

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(@D)
	@$(CC) $(IOS_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.m
	@mkdir -p $(@D)
	@$(CC) $(IOS_CFLAGS) -fobjc-arc -c $< -o $@

# ---------------- App 图标 ----------------

icons: $(BUILD_DIR)/mkicon
	@mkdir -p $(ICON_DIR)
	@for spec in $(ICON_SPECS); do \
		name=$${spec%%:*}; size=$${spec##*:}; out=$(ICON_DIR)/$$name; \
		if [ -f "$$out" ] && [ ! "$$out" -ot "$(BUILD_DIR)/mkicon" ] \
			&& { [ ! -f "$(APPICON_SRC)" ] || [ ! "$$out" -ot "$(APPICON_SRC)" ]; }; then \
			continue; \
		fi; \
		if [ -f "$(APPICON_SRC)" ]; then \
			$(BUILD_DIR)/mkicon "$$out" "$$size" "$(APPICON_SRC)" >/dev/null; \
		else \
			$(BUILD_DIR)/mkicon "$$out" "$$size" >/dev/null; \
		fi; \
	done
	@echo "  ICON  $(ICON_DIR)/"

$(BUILD_DIR)/mkicon: $(TOOLS_DIR)/mkicon.c
	@mkdir -p $(BUILD_DIR)
	@$(CC) -O2 -Wall -o $@ $< -framework CoreGraphics -framework ImageIO -framework CoreFoundation

# 图标源图来自 macOS 自带的 Terminal.app（Apple 的美术资源，仅限本机自用）
appicon:
	@mkdir -p $(BUILD_DIR) $(ASSET_DIR)
	@rm -rf $(BUILD_DIR)/Terminal.iconset
	iconutil -c iconset /System/Applications/Utilities/Terminal.app/Contents/Resources/Terminal.icns \
		-o $(BUILD_DIR)/Terminal.iconset
	@cp $(BUILD_DIR)/Terminal.iconset/icon_128x128@2x.png $(APPICON_SRC)
	@rm -rf $(BUILD_DIR)/Terminal.iconset
	@echo "  ICON  $(APPICON_SRC)"

storyboard:
	@rm -rf $(LAUNCHSCREEN)
	xcrun ibtool --compile $(LAUNCHSCREEN) \
		--target-device iphone --target-device ipad \
		--minimum-deployment-target $(MIN_IOS) $(SRC_DIR)/ios/LaunchScreen.storyboard
	@echo "  STORY $(LAUNCHSCREEN)"

# ---------------- 主机端 ----------------

test: $(BUILD_DIR)/test_vt
	@$(BUILD_DIR)/test_vt

$(BUILD_DIR)/test_vt: $(TEST_DIR)/test_vt.c $(CORE_SRC)
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c99 -g -O1 -Wall -Wno-unused-parameter -I$(SRC_DIR)/core -o $@ $^

dump: $(BUILD_DIR)/termdump

$(BUILD_DIR)/termdump: $(TOOLS_DIR)/termdump.c $(CORE_SRC) $(HOST_SRC)
	@mkdir -p $(BUILD_DIR)
	@$(CC) -std=c99 -g -O1 -Wall -Wno-unused-parameter \
		-I$(SRC_DIR)/core -I$(SRC_DIR)/host -o $@ $^

# ---------------- 真机 ----------------

install: ios
	@tar -czf $(BUILD_DIR)/$(APP_NAME).tar.gz -C $(OUT_DIR) $(APP_NAME).app
	@scp $(BUILD_DIR)/$(APP_NAME).tar.gz $(DEVICE):/tmp/
	@ssh $(DEVICE) 'set -e; P=/Applications; [ -d /var/jb ] && P=/var/jb/Applications; \
		rm -rf $$P/$(APP_NAME).app; tar -xzf /tmp/$(APP_NAME).tar.gz -C $$P; \
		uicache -a >/dev/null 2>&1 || uicache -p $$P/$(APP_NAME).app >/dev/null 2>&1 || true; \
		echo "  APP   $$P/$(APP_NAME).app"'

clean:
	@rm -rf $(BUILD_DIR) $(OUT_DIR)
	@echo "  CLEAN $(BUILD_DIR)/ $(OUT_DIR)/"
