#!/bin/sh
#
# 把 out/Terminal.app 打成 ipa（Payload/Terminal.app），给 TrollStore、AltStore
# 这类安装方式使用。产物不带 App Store 签名，仅供越狱设备自用。
#
#   - 同样的源码总是产出同样的 ipa（时间戳归一化 + 不写扩展属性）
#
# 一般由 `make ipa` 调用，也可以手动执行：
#   sh packaging/mkipa.sh
#
set -e

APP_NAME=${APP_NAME:-Terminal}
VERSION=${VERSION:-1.0.2}

APP_DIR=${APP_DIR:-out/$APP_NAME.app}
OUT_IPA=${OUT_IPA:-out/${APP_NAME}_${VERSION}.ipa}
WORK=${WORK:-build/ipa}

if [ ! -d "$APP_DIR" ]; then
    echo "找不到 $APP_DIR，先跑 make ios" >&2
    exit 1
fi

rm -rf "$WORK"
mkdir -p "$WORK/Payload"
cp -R "$APP_DIR" "$WORK/Payload/"
find "$WORK/Payload" -name '.DS_Store' -delete 2>/dev/null || true

# 归一化时间戳：同样的源码总是产出同样的 ipa
find "$WORK" -exec touch -h -t 200001010000.00 {} + 2>/dev/null || true

mkdir -p "$(dirname "$OUT_IPA")"
rm -f "$OUT_IPA"
ABS_IPA=$(cd "$(dirname "$OUT_IPA")" && pwd)/$(basename "$OUT_IPA")

if command -v zip >/dev/null 2>&1; then
    # -X 不写扩展属性，否则归档里会带上机器相关的 uid/gid 与时间戳
    ( cd "$WORK" && zip -qry -X "$ABS_IPA" Payload )
elif command -v ditto >/dev/null 2>&1; then
    ( cd "$WORK" && ditto -c -k --sequesterRsrc --keepParent Payload "$ABS_IPA" )
else
    echo "既没有 zip 也没有 ditto，无法打包 ipa" >&2
    exit 1
fi

echo "  IPA   $OUT_IPA  ($(du -h "$OUT_IPA" | cut -f1))"
