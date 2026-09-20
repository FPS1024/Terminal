#!/bin/sh
#
# 把 build/Terminal.app 打成 rootless 越狱用的 .deb。
#
#   - 只产出 rootless 一种包：安装到 /var/jb/Applications/Terminal.app
#   - 架构标记为 iphoneos-arm64（Dopamine / Palera1n 等 rootless 越狱）
#   - 构建机上没有 dpkg，所以自己拼 ar 归档，不依赖 dpkg-deb
#   - 归档不带 gzip 时间戳，同样的输入会得到同样的包（方便 Release 校验）
#
# 一般由 `make deb` 调用，也可以手动执行：
#   sh packaging/mkdeb.sh
#
set -e

APP_NAME=${APP_NAME:-Terminal}
# deb 的包名（Sileo / dpkg 里认的那个标识）。不用 bundle id：
# bundle id 是 App 自己的事，包名越短越干净，Sileo 里的升级判断也只看它。
PACKAGE=${PACKAGE:-terminal}
# 老版本的包名，用来把旧包顶掉（改名之后不写 Replaces 的话，
# 新包会被 dpkg 判成"想覆盖别人拥有的文件"而装不上）
OLD_PACKAGE=${OLD_PACKAGE:-com.malacaihongpi.terminal}
VERSION=${VERSION:-1.0.5}
DEB_ARCH=${DEB_ARCH:-iphoneos-arm64}
MAINTAINER=${MAINTAINER:-FPS1024 <ceaser.k.w@outlook.com>}

APP_DIR=${APP_DIR:-build/$APP_NAME.app}
OUT_DEB=${OUT_DEB:-out/${APP_NAME}_${VERSION}_${DEB_ARCH}.deb}
WORK=${WORK:-build/deb}

PREFIX=/var/jb                      # rootless 前缀，dpkg 会把它映射到真实 jb 目录
APP_DEST="$PREFIX/Applications"     # 真实安装路径：/var/jb/Applications

if [ ! -d "$APP_DIR" ]; then
    echo "找不到 $APP_DIR，先跑 make ios" >&2
    exit 1
fi

rm -rf "$WORK"
mkdir -p "$WORK/control" "$WORK/root$APP_DEST"

# ---- 有效载荷 ----
cp -R "$APP_DIR" "$WORK/root$APP_DEST/"
find "$WORK/root" -name '.DS_Store' -delete 2>/dev/null || true

# ---- control ----
SIZE=$(du -sk "$WORK/root" | awk '{print $1}')
cat > "$WORK/control/control" <<CONTROL
Package: $PACKAGE
Name: $APP_NAME
Version: $VERSION
Architecture: $DEB_ARCH
Description: 越狱设备上的终端 App
 支持 UTF-8 / 中文输入(系统拼音候选条) / 中文与 emoji 宽度对齐 /
 256 色与真彩色 / 回滚与 reflow / 多会话标签 / 硬件键盘与快捷键条。
Maintainer: $MAINTAINER
Author: $MAINTAINER
Section: Utilities
Priority: optional
Installed-Size: $SIZE
Replaces: $OLD_PACKAGE
Conflicts: $OLD_PACKAGE
Depends: firmware (>= 12.0)
CONTROL

cat > "$WORK/control/postinst" <<'POSTINST'
#!/bin/sh
set -e
JB=""
[ -d /var/jb ] && JB=/var/jb
APP="$JB/Applications/Terminal.app"
[ -d "$APP" ] || APP=/Applications/Terminal.app
for U in "$JB/usr/bin/uicache" /usr/bin/uicache; do
    if [ -x "$U" ]; then
        "$U" -a >/dev/null 2>&1 || "$U" -p "$APP" >/dev/null 2>&1 || true
        break
    fi
done
killall -HUP SpringBoard >/dev/null 2>&1 || true
exit 0
POSTINST

cat > "$WORK/control/postrm" <<'POSTRM'
#!/bin/sh
set -e
JB=""
[ -d /var/jb ] && JB=/var/jb
for U in "$JB/usr/bin/uicache" /usr/bin/uicache; do
    [ -x "$U" ] && { "$U" -a >/dev/null 2>&1 || true; break; }
done
killall -HUP SpringBoard >/dev/null 2>&1 || true
exit 0
POSTRM
chmod 0755 "$WORK/control/postinst" "$WORK/control/postrm"

# ---- tar 参数：bsdtar 与 GNU tar 的归属写法不同，实测一次 ----
set -- --format=gnutar --uid 0 --gid 0 --uname root --gname wheel --options gzip:!timestamp
if ! LC_ALL=C tar "$@" -czf /dev/null "$WORK/control/control" 2>/dev/null; then
    set -- --format=gnutar --owner=0 --group=0
fi
TARFLAGS="$*"

# ---- 归一化时间戳：同样的源码总是产出同样的 deb ----
stamp_mtimes() { find "$1" -exec touch -h -t 200001010000.00 {} + 2>/dev/null || true; }
stamp_mtimes "$WORK"

# ---- deb 的三个成员 ----
export COPYFILE_DISABLE=1
( cd "$WORK/control" && LC_ALL=C tar $TARFLAGS -czf ../control.tar.gz . )
( cd "$WORK/root"    && LC_ALL=C tar $TARFLAGS -czf ../data.tar.gz . )
printf '2.0\n' > "$WORK/debian-binary"
stamp_mtimes "$WORK"      # 三个成员的 mtime 会被写进 ar 头，也要归一化

# ---- ar 归档 ----
mkdir -p "$(dirname "$OUT_DEB")"
rm -f "$OUT_DEB"
ABS_DEB=$(cd "$(dirname "$OUT_DEB")" && pwd)/$(basename "$OUT_DEB")
( cd "$WORK" && ar rc "$ABS_DEB" debian-binary control.tar.gz data.tar.gz )

echo "  DEB   $OUT_DEB  ($(du -h "$OUT_DEB" | cut -f1))  安装到 $APP_DEST/$APP_NAME.app"
