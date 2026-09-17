#!/bin/sh
#
# 在 macOS 上直接把 App 打成 .deb（这台机器没有 dpkg，所以自己拼 ar 归档）
#
#   ROOTLESS=1 sh packaging/mkdeb.sh     # 无根越狱(Dopamine/Palera1n)，装到 /var/jb
#   sh packaging/mkdeb.sh                # 传统有根越狱，装到 /Applications
#
set -e

APP_NAME=${APP_NAME:-Terminal}
BUNDLE_ID=${BUNDLE_ID:-com.malacaihongpi.terminal}
VERSION=${VERSION:-1.0.0}
ROOTLESS=${ROOTLESS:-0}
BUILD=${BUILD:-build}
OUT=${OUT:-dist}

if [ "$ROOTLESS" = "1" ]; then
    PREFIX=/var/jb
    DEBARCH=iphoneos-arm64
else
    PREFIX=
    DEBARCH=iphoneos-arm
fi

APP_SRC="$BUILD/$APP_NAME.app"
if [ ! -d "$APP_SRC" ]; then
    echo "找不到 $APP_SRC，先跑 make ios" >&2
    exit 1
fi

WORK="$OUT/pkg"
rm -rf "$WORK"
mkdir -p "$WORK/control" "$WORK/root$PREFIX/Applications"

# ---- 有效载荷 ----
cp -R "$APP_SRC" "$WORK/root$PREFIX/Applications/"
find "$WORK/root" -name '.DS_Store' -delete 2>/dev/null || true

# ---- control ----
SIZE=$(du -sk "$WORK/root" | awk '{print $1}')
cat > "$WORK/control/control" <<EOF
Package: $BUNDLE_ID
Name: Terminal
Version: $VERSION
Architecture: $DEBARCH
Description: 越狱设备上的终端 App
 支持 UTF-8 / 中文输入(系统拼音候选条) / 中文与 emoji 宽度对齐 /
 256 色与真彩色 / 回滚与 reflow / 多会话标签 / 硬件键盘与快捷键条。
Maintainer: malacaihongpi
Author: malacaihongpi
Section: Utilities
Priority: optional
Installed-Size: $SIZE
Depends: firmware (>= 12.0)
EOF

cat > "$WORK/control/postinst" <<'EOF'
#!/bin/sh
set -e
JB=""
[ -d /var/jb ] && JB=/var/jb
APP="$JB/Applications/Terminal.app"
[ -d "$APP" ] || APP=/Applications/Terminal.app
for U in "$JB/usr/bin/uicache" /usr/bin/uicache "$JB/var/jb/usr/bin/uicache"; do
    if [ -x "$U" ]; then
        "$U" -a >/dev/null 2>&1 || "$U" -p "$APP" >/dev/null 2>&1 || true
        break
    fi
done
killall -HUP SpringBoard >/dev/null 2>&1 || true
exit 0
EOF

cat > "$WORK/control/postrm" <<'EOF'
#!/bin/sh
set -e
JB=""
[ -d /var/jb ] && JB=/var/jb
for U in "$JB/usr/bin/uicache" /usr/bin/uicache; do
    [ -x "$U" ] && { "$U" -a >/dev/null 2>&1 || true; break; }
done
killall -HUP SpringBoard >/dev/null 2>&1 || true
exit 0
EOF
chmod 0755 "$WORK/control/postinst" "$WORK/control/postrm"

# ---- 三个 tar 成员 ----
TARFLAGS="--format=gnutar --uid 0 --gid 0 --uname root --gname wheel"
if ! tar $TARFLAGS -cf /dev/null . 2>/dev/null; then
    TARFLAGS="--format=gnutar --owner=0 --group=0"
fi

export COPYFILE_DISABLE=1
( cd "$WORK/control" && tar $TARFLAGS -czf ../control.tar.gz . )
( cd "$WORK/root"    && tar $TARFLAGS -czf ../data.tar.gz . )
printf '2.0\n' > "$WORK/debian-binary"

# ---- ar 归档 ----
mkdir -p "$OUT"
DEB="$OUT/${APP_NAME}_${VERSION}_${DEBARCH}.deb"
rm -f "$DEB"
ABS=$(cd "$OUT" && pwd)
( cd "$WORK" && ar rc "$ABS/$(basename "$DEB")" debian-binary control.tar.gz data.tar.gz )

echo "DEB -> $DEB"
ls -lh "$DEB" | awk '{print "     size:", $5}'
