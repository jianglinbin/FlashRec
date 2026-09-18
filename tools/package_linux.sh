#!/usr/bin/env bash
# FlashRec Linux 打包（d96/d97）—— 一次 Release 构建，产出 .deb 与 .AppImage。
#
# 用法（在仓库根目录跑）：
#   bash tools/package_linux.sh                  # 全量：配置 → 构建 → deb + AppImage
#   bash tools/package_linux.sh --only deb       # 只出 .deb
#   bash tools/package_linux.sh --only appimage  # 只出 .AppImage
#   bash tools/package_linux.sh --skip-build     # 复用 build-pkg/ 已有产物，只重打包
#   bash tools/package_linux.sh -j 8             # 并行度（默认 nproc）
#   bash tools/package_linux.sh --smoke          # 额外跑一次 AppImage 冒烟（需先关掉其他实例）
#   bash tools/package_linux.sh --clean          # 先删 build-pkg/ 再全量重来
#
# 产物：dist/flashrec_<ver>_<arch>.deb
#       dist/FlashRec-<ver>-x86_64.AppImage
#
# 为什么 deb 用 CPack、AppImage 不用（两条不同路子）：
#   · deb 的难点是**依赖推导**（少写一个 libX11 就装完起不来），dpkg-shlibdeps 是权威，
#     CPack 的 DEB 生成器直接代跑它 —— 用 CPack 划算。
#   · AppImage 的难点是**捆绑粒度与 AppRun**：要按 excludelist 剔掉驱动/glibc、要
#     给 AppRun 塞桌面项自注册（d93 的图标依赖它）、要控制 AppDir 顶层文件命名。
#     CPack 的 AppImage 生成器这些都管不了，所以直连 linuxdeploy + appimagetool。
#
# 关键前提（缺一个就出废包）：
#   1. CMakeLists.txt 的 UNIX 分支必须是 FHS 布局（bin/ + share/flashrec/assets）。
#   2. src/platform/paths.cpp 必须带 "<exe>/../share/flashrec/assets" 搜索位，
#      否则装完 exe 找不到 assets（SCPD/图标全丢，DMR 直接起不来）。
#   3. -DCMAKE_INSTALL_PREFIX 是 configure 期定的：.desktop 里的 Exec 写死成
#      <prefix>/bin/flashrec，所以这里**不用 `cmake --install --prefix`**，
#      而是 configure 定 /usr + 安装时用 DESTDIR 暂存（Debian 标准做法）。
#
# 外部工具：patchelf 必须预装（linuxdeploy 用它改 rpath）。linuxdeploy 与 appimagetool
#           首次运行自动下载到 dist/.tools/ 并复用，之后离线可跑。

set -euo pipefail

# ---------------------------------------------------------------- 参数与路径

ONLY="both"
SKIP_BUILD=0
DO_CLEAN=0
DO_SMOKE=0
JOBS="$(nproc 2>/dev/null || echo 4)"

while [ $# -gt 0 ]; do
  case "$1" in
    --only)       ONLY="${2:-}"; shift 2 ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --clean)      DO_CLEAN=1; shift ;;
    --smoke)      DO_SMOKE=1; shift ;;
    -j)           JOBS="${2:-}"; shift 2 ;;
    -h|--help)    sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "未知参数：$1（-h 看用法）" >&2; exit 2 ;;
  esac
done
case "$ONLY" in both|deb|appimage) ;; *) echo "--only 只能是 both/deb/appimage" >&2; exit 2 ;; esac

ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
cd "$ROOT"

case "$(uname -s)" in
  Linux) ;;
  *) echo "此脚本只能在 Linux 上跑（deb 依赖推导与 AppImage 工具都是 Linux 专属）" >&2; exit 1 ;;
esac

BUILD_DIR="$ROOT/build-pkg"
DIST="$ROOT/dist"
TOOLS_DIR="$DIST/.tools"
APPIMG_WORK="$DIST/.appimage"
APPDIR="$APPIMG_WORK/AppDir"
APP_ID="com.flashrec.FlashRec"
ICON_SRC="$ROOT/assets/icons/app_256.png"

# 所有日志走 stderr：脚本里多处用 $(...) 取函数返回值（如 fetch_tool 回传工具路径），
# 日志若进 stdout 会被一起捕获进去 —— 直接全塞 stderr 从根上避免这类污染。
log()  { printf '\033[1;36m[pack]\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33m[pack]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[pack]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- 前置检查

need_tool() {
  command -v "$1" >/dev/null 2>&1 || die "缺 $1 —— 请先装（$2）"
}

if [ "$ONLY" != "deb" ]; then
  # linuxdeploy 用 patchelf 往捆绑的 so 与主 exe 上写 $ORIGIN rpath；没有它 bundling 是废的
  command -v patchelf >/dev/null 2>&1 || \
    die "缺 patchelf（AppImage 捆绑必需）—— sudo apt-get install -y patchelf
     只想出 deb 的话加 --only deb"
fi
need_tool cmake "apt-get install -y cmake"
need_tool ninja "apt-get install -y ninja-build"

# AppImage 工具本身是 AppImage：type-2 runtime 默认要 libfuse.so.2，而新发行版常常
# 只装了 fuse3，所以统一用 APPIMAGE_EXTRACT_AND_RUN=1 让工具自解压后跑，彻底绕开 FUSE。
# （这是 AppImage runtime 自己的环境变量，不是本脚本自造的。）
export APPIMAGE_EXTRACT_AND_RUN=1

fetch_tool() {
  local name="$1" url="$2" path="$TOOLS_DIR/$1"
  if [ -x "$path" ]; then echo "$path"; return 0; fi
  mkdir -p "$TOOLS_DIR"
  log "下载 $name（缓存在 dist/.tools/，之后离线复用）"
  wget -q -O "$path" "$url" || die "$name 下载失败：$url"
  chmod +x "$path"
  echo "$path"
}

# ---------------------------------------------------------------- 1. 构建

if [ "$DO_CLEAN" = 1 ]; then
  log "清理 $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

if [ "$SKIP_BUILD" = 0 ]; then
  log "配置（prefix=/usr —— deb 的根，且会写进 .desktop 的 Exec）"
  cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        >/dev/null
  log "构建（-j$JOBS）"
  cmake --build "$BUILD_DIR" -j "$JOBS"
else
  [ -d "$BUILD_DIR" ] || die "--skip-build 但 $BUILD_DIR 不存在"
  log "跳过构建（--skip-build）"
fi

[ -x "$BUILD_DIR/bin/flashrec" ] || die "没找到 $BUILD_DIR/bin/flashrec，构建没成功"
[ -f "$BUILD_DIR/$APP_ID.desktop" ] || die "没找到 $BUILD_DIR/$APP_ID.desktop（cmake/linux_desktop.cmake 没生效？）"

VERSION="$(sed -n 's/^project(FlashRec VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -1 || true)"
[ -n "$VERSION" ] || die "从 CMakeLists.txt 里读不到 project(VERSION)"
log "版本 $VERSION"

mkdir -p "$DIST"

# ---------------------------------------------------------------- 2. .deb

build_deb() {
  log "打包 .deb"
  need_tool cpack    "apt-get install -y cmake"
  need_tool dpkg-deb "apt-get install -y dpkg"
  [ -n "$(command -v dpkg-shlibdeps || true)" ] || \
    warn "没有 dpkg-shlibdeps，自动依赖推导会失败；可加 -DFLASHREC_DEB_AUTO_SHLIBDEPS=OFF 手工给依赖"

  # CPack 自己会往 _CPack_Packages/ 里做一次完整 install，不用我们手工 DESTDIR
  ( cd "$BUILD_DIR" && cpack --config CPackConfig.cmake -G DEB -B "$DIST" )

  local deb
  deb="$(ls -1t "$DIST"/flashrec_*.deb 2>/dev/null | head -1 || true)"
  [ -n "$deb" ] || die "没生成 .deb（看上面 cpack 的输出）"

  log "deb 控制信息："
  dpkg-deb -f "$deb" Package Version Architecture Depends Recommends 2>/dev/null | sed 's/^/    /'
  log "deb 内容："
  dpkg-deb -c "$deb" | awk '{printf "    %8s  %s\n", $3, $6}'
  log "→ $deb（$(du -h "$deb" | cut -f1)）"
}

# ---------------------------------------------------------------- 3. .AppImage

# AppRun：AppImage 的入口。用自写的而不是 linuxdeploy 默认的那个，只为多干一件事——
# 把桌面项注册到用户目录。原因见 d93：Wayland 下窗口图标/名字/归组**完全**由合成器按
# 窗口 app_id 匹配同名桌面项得到，而 AppImage 不会自动注册桌面项（那是 appimaged /
# AppImageLauncher 的活），不注册就永远是通用图标。
# 写法上刻意保守：
#   · 幂等 —— 内容一致就不写；且用 X-FlashRec-AppImage= 标记行认领，遇到没有该标记的
#     同名桌面项（例如用户从 deb 复制过来的）直接放弃，绝不覆盖别人的安装。
#   · 可关 —— FLASHREC_NO_DESKTOP_REGISTER=1 时完全不动用户目录。
#   · Exec 指向当前 .AppImage 自身（$APPIMAGE 是 AppImage runtime 注入的绝对路径），
#     所以 AppImage 被挪走后再跑一次会自我修复。
write_apprun() {
  cat > "$APPDIR/AppRun" <<'APPRUN'
#!/bin/sh
# FlashRec AppImage 入口（由 tools/package_linux.sh 生成）
set -eu

HERE="$(dirname "$(readlink -f "$0")")"
export APPDIR="$HERE"
export PATH="$HERE/usr/bin:$PATH"

APP_ID="com.flashrec.FlashRec"
SELF="${APPIMAGE:-$(readlink -f "$0")}"

if [ -z "${FLASHREC_NO_DESKTOP_REGISTER:-}" ]; then
  SRC_DESK="$HERE/usr/share/applications/$APP_ID.desktop"
  SRC_ICON="$HERE/usr/share/icons/hicolor/256x256/apps/$APP_ID.png"
  DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
  DST_DESK="$DATA/applications/$APP_ID.desktop"
  DST_ICON="$DATA/icons/hicolor/256x256/apps/$APP_ID.png"

  # 认领检查：目标已存在且不是我们写的（无标记行）⇒ 不碰
  if [ ! -e "$DST_DESK" ] || grep -q '^X-FlashRec-AppImage=' "$DST_DESK" 2>/dev/null; then
    if [ -f "$SRC_DESK" ]; then
      if mkdir -p "$(dirname "$DST_DESK")" 2>/dev/null; then
        tmp="$DST_DESK.$$"
        # Exec 重写成 .AppImage 自身：AppImage 内的 Exec 是裸名 flashrec，
        # 直接注册进菜单会启动失败。
        awk -v self="$SELF" '
          /^Exec=/ { print "Exec=\"" self "\""; next }
          { print }
          END { print "X-FlashRec-AppImage=" self }
        ' "$SRC_DESK" > "$tmp" 2>/dev/null && mv -f "$tmp" "$DST_DESK" 2>/dev/null || rm -f "$tmp"
      fi
    fi
    if [ -f "$SRC_ICON" ]; then
      mkdir -p "$(dirname "$DST_ICON")" 2>/dev/null || true
      cp -f "$SRC_ICON" "$DST_ICON" 2>/dev/null || true
    fi
  fi
fi

exec "$HERE/usr/bin/flashrec" "$@"
APPRUN
  chmod +x "$APPDIR/AppRun"
}

# AppDir 顶层必须有的三件套：入口脚本、桌面项、图标（appimagetool 要求"有且只有一个
# *.desktop"，且 Icon= 指向的文件能在顶层找到）。桌面项文件名保持 app_id —— 将来被
# 桌面集成工具注册后与窗口 app_id 同名，归组/图标才对得上（d93）。
write_top_level() {
  sed -e 's|^Exec=.*|Exec=flashrec|' "$BUILD_DIR/$APP_ID.desktop" > "$APPDIR/$APP_ID.desktop"
  cp -f "$ICON_SRC" "$APPDIR/$APP_ID.png"
  ln -sf "$APP_ID.png" "$APPDIR/.DirIcon"
  # AppRun 的自注册要读这份（Exec 是 /usr/bin/flashrec，运行时会被改写成 .AppImage 路径）
  mkdir -p "$APPDIR/usr/share/applications"
  cp -f "$BUILD_DIR/$APP_ID.desktop" "$APPDIR/usr/share/applications/$APP_ID.desktop"
  write_apprun
}

build_appimage() {
  log "打包 .AppImage"
  need_tool wget "apt-get install -y wget"

  local linuxdeploy appimagetool
  linuxdeploy="$(fetch_tool linuxdeploy-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage)"
  appimagetool="$(fetch_tool appimagetool-x86_64.AppImage \
    https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage)"

  # ---- AppDir：DESTDIR 暂存 install（与 deb 同一个 prefix=/usr，路径天然对齐）----
  rm -rf "$APPDIR"
  DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" >/dev/null
  [ -x "$APPDIR/usr/bin/flashrec" ] || die "AppDir 里没有 usr/bin/flashrec"

  # 先立自己的入口/桌面项/图标，再交给 linuxdeploy 只做"收依赖"这一件事
  write_top_level

  # ---- 捆绑依赖 ----
  # linuxdeploy 按主 exe 的 DT_NEEDED 递归收集 so，并按 AppImage excludelist 剔掉
  # glibc / 驱动 / 图形栈这类"必须用宿主机的"库；同时 patchelf 把 rpath 改成
  # $ORIGIN 相对路径。这就是"全捆绑"的实现点。
  # 刻意**不传** --desktop-file / --icon-file：linuxdeploy 会按传入的文件名重写
  # 桌面项的 Icon= 与顶层图标命名，一改就和 app_id 对不上了（d93 的整套匹配失效）。
  # 注意 GLFW 的 wayland/x11 后端是 dlopen 的，ldd 看不到 —— 那些库本来就在
  # excludelist 里（宿主机桌面必有），不需要也不应该捆。
  log "linuxdeploy 收集依赖（全捆绑；剔除了什么看下面）"
  local ldl_log="$APPIMG_WORK/linuxdeploy.log"
  mkdir -p "$APPIMG_WORK"
  if ! "$linuxdeploy" --appdir "$APPDIR" --executable "$APPDIR/usr/bin/flashrec" \
        >"$ldl_log" 2>&1; then
    sed 's/^/    /' "$ldl_log" >&2
    die "linuxdeploy 失败（完整日志 $ldl_log）"
  fi
  grep -iE 'exclud|not found|WARNING|ERROR' "$ldl_log" | sed 's/^/    /' >&2 || true

  # linuxdeploy 会覆盖 AppRun、可能顺手改桌面项/图标 —— 全部重新写回，保证以我方为准
  write_top_level

  local nlib
  nlib="$(find "$APPDIR/usr/lib" -maxdepth 1 -name '*.so*' 2>/dev/null | wc -l || true)"
  log "已捆绑 $nlib 个 so（$(du -sh "$APPDIR/usr/lib" 2>/dev/null | cut -f1 || echo -)）"

  # ---- 出 AppImage ----
  local out="$DIST/FlashRec-$VERSION-x86_64.AppImage"
  rm -f "$out"
  if ! ARCH=x86_64 "$appimagetool" "$APPDIR" "$out" >"$APPIMG_WORK/appimagetool.log" 2>&1; then
    sed 's/^/    /' "$APPIMG_WORK/appimagetool.log" >&2
    die "appimagetool 失败"
  fi
  [ -f "$out" ] || die "appimagetool 没产出 AppImage"
  chmod +x "$out"

  # ---- 自检：解包后 ldd 一遍，确认没有 "not found" ----
  # 不启动 GUI（起 GUI 会与已在跑的实例抢单实例锁）；要真启动得显式加 --smoke
  log "自检：解包后检查动态库可解析性"
  local check="$APPIMG_WORK/check"
  rm -rf "$check"; mkdir -p "$check"
  if ( cd "$check" && "$out" --appimage-extract >/dev/null 2>&1 ); then
    local missing
    missing="$(cd "$check/squashfs-root" && \
      LD_LIBRARY_PATH="$check/squashfs-root/usr/lib" \
      ldd usr/bin/flashrec 2>/dev/null | grep 'not found' || true)"
    if [ -n "$missing" ]; then
      warn "有未解析的动态库（宿主机会兜底，但说明捆绑有缺口）："
      echo "$missing" | sed 's/^/    /' >&2
    else
      log "自检通过：AppImage 内所有 DT_NEEDED 均可解析"
    fi
  else
    warn "解包自检跳过（--appimage-extract 没成功）"
  fi
  rm -rf "$check"

  if [ "$DO_SMOKE" = 1 ]; then
    log "冒烟：限时启动 6s（应用是单实例的，先关掉别的实例才准）"
    set +e
    timeout 6 "$out" >/dev/null 2>&1
    local rc=$?
    set -e
    if [ "$rc" = 124 ]; then
      log "冒烟通过：跑满 6s 被 timeout 收掉，说明没崩"
    else
      warn "冒烟异常：退出码 $rc（124=正常；0/其他=可能被单实例挡下或起不来）"
    fi
  fi

  log "→ $out（$(du -h "$out" | cut -f1)）"
}

# ---------------------------------------------------------------- 主流程

case "$ONLY" in
  deb)      build_deb ;;
  appimage) build_appimage ;;
  both)     build_deb; echo >&2; build_appimage ;;
esac

echo >&2
log "完成。dist/ 产物："
shopt -s nullglob
for f in "$DIST"/*.deb "$DIST"/*.AppImage; do
  printf '    %-8s %s\n' "$(du -h "$f" | cut -f1)" "$(basename "$f")" >&2
done
