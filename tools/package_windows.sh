#!/usr/bin/env bash
# FlashRec Windows 打包 —— 一次 Release 构建（关控制台），产出便携 ZIP + exe 安装包。
#
# 用法（在仓库根目录的 Git Bash 里跑）：
#   bash tools/package_windows.sh                # 全量：配置 → 构建 → 出 ZIP + exe
#   bash tools/package_windows.sh --only zip     # 只出便携 ZIP
#   bash tools/package_windows.sh --only exe     # 只出 exe 安装包
#   bash tools/package_windows.sh --skip-build   # 复用 build-rel/ 已有产物，只重打包
#   bash tools/package_windows.sh --clean        # 先删 build-rel/ 再全量重来
#   bash tools/package_windows.sh -j 8           # 并行度（默认 nproc）
#   bash tools/package_windows.sh --smoke        # 额外冒烟：解压后限时启动，再用 quit_app.py 优雅收掉
#
# 产物：dist/FlashRec-<ver>-win64.zip
#       解压得到一个同名目录，内含 flashrec.exe + libmpv-2.dll + VC++ 运行库 + assets/，
#       解压即用。
#       dist/FlashRec-<ver>-win64-setup.exe
#       标准安装向导（默认免管理员权限），装完有开始菜单项 + 可选桌面快捷方式 +
#       「添加/删除程序」卸载项。脚本模板 tools/installer_windows.iss，工具链 Inno Setup 6
#       （首次自动下载便携版到 dist/.tools/innosetup/ 并复用，见 ensure_inno）。
#       （命名与 Linux 侧对齐：flashrec_<ver>_amd64.deb / FlashRec-<ver>-x86_64.AppImage）
#
# 为什么单独一个构建目录 build-rel，而不复用开发用的 build/：
#   FLASHREC_CONSOLE 已默认 OFF（见 CMakeLists.txt），build/ 与 build-rel 都是 Windows
#   GUI 子系统（双击不弹控制台）；运行时用 `flashrec --console` 才显示控制台。
#   仍分目录的理由：① 发布与开发构建隔离，避免 dev 调试开关/产物混入；
#   ② 不必先关掉正在跑的实例 —— 输出路径不同，不会撞 LNK1104。
#   脚本仍显式传 -DFLASHREC_CONSOLE=OFF，作为"发布必须无控制台"的冗余保险。
#
# 为什么打包文件清单用 cmake --install，而不是手工 cp 三样：
#   CMakeLists.txt 的 WIN32 分支已声明便携布局（exe + libmpv-2.dll + assets/），
#   安装规则是唯一真值；手工再罗列一份迟早和它漂移（加了新资源目录就会漏）。
#
# 依赖：除 MSVC/Ninja/CMake 外，压缩用 python（Git Bash 默认不带 zip 命令）；
#       python 缺失时回退到 Windows 自带的 tar.exe（bsdtar，-a 可直接写 zip）。
#       全程不依赖 PowerShell（在本项目的工具环境里它会被安全策略拦下）。

set -euo pipefail

# ---------------------------------------------------------------- 参数与路径

JOBS="$(nproc 2>/dev/null || echo 4)"
SKIP_BUILD=0
DO_CLEAN=0
DO_SMOKE=0
ONLY="both"          # both | zip | exe —— 与 tools/package_linux.sh 的 --only 同形

while [ $# -gt 0 ]; do
  case "$1" in
    --only)       ONLY="${2:-}"; shift 2 ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --clean)      DO_CLEAN=1; shift ;;
    --smoke)      DO_SMOKE=1; shift ;;
    -j)           JOBS="${2:-}"; shift 2 ;;
    -h|--help)    sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "未知参数：$1（-h 看用法）" >&2; exit 2 ;;
  esac
done
case "$ONLY" in both|zip|exe) ;; *) echo "--only 只能是 both/zip/exe" >&2; exit 2 ;; esac

ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
cd "$ROOT"

# 日志全走 stderr：脚本里多处用 $(...) 取函数返回值，日志若进 stdout 会被一起捕获进去。
log()  { printf '\033[1;36m[pack]\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33m[pack]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[pack]\033[0m %s\n' "$*" >&2; exit 1; }

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) ;;
  *) die "此脚本只能在 Windows（Git Bash）上跑；Linux 打包请用 tools/package_linux.sh" ;;
esac

BUILD_DIR="$ROOT/build-rel"
DIST="$ROOT/dist"
PKG_WORK="$DIST/.winpkg"

# cmake / ninja / python 都是原生 Windows 程序，**不认** MSYS 的 "/d/dev/..." 形态路径
# （实测：cmake 会直接报 source directory "/d/dev/FlashRec" does not exist，且 Git Bash
# 的自动路径转换在这里并不可靠）。所以凡是要交给它们的路径，一律先转成 "D:/dev/..." —
# 混合模式（cygpath -m）用的是正斜杠，Windows 工具与 bash 两头都认，且不必担心反斜杠转义。
# Bash 自己用的路径（rm/[-d]）保持 MSYS 形态不变。
win_path() { cygpath -m "$1"; }

# ---------------------------------------------------------------- MSVC 环境

# msvc_env.sh 只导出绝对路径变量（FR_CMAKE / FR_NINJA / FR_MSVC），原因见该文件里的说明：
# "C:/..." 风格路径塞进 bash 的 PATH 会被冒号切坏，所以**不要**靠 PATH 找 cl/dumpbin。
[ -f "$ROOT/tools/msvc_env.sh" ] || die "缺 tools/msvc_env.sh"
# shellcheck disable=SC1091
. "$ROOT/tools/msvc_env.sh"
[ -x "$FR_CMAKE" ] || die "cmake 不可用：$FR_CMAKE"
[ -x "$FR_NINJA" ] || die "ninja 不可用：$FR_NINJA"

# 编译器与构建工具**显式指定**，不靠 PATH 查找。
# 三条理由：① 与本仓库既有 build/ 的配法一致（它的 CMakeCache 里 CMAKE_C_COMPILER /
# CXX_COMPILER / RC_COMPILER / MT 都是写死的绝对路径）；② 打包脚本要能在任意 shell、
# 任意 PATH 状态下稳定跑；③ rc/mt 尤其隐蔽 —— CMake 找不到它们时症状是"编译过了、链接挂"
# （CMAKE_MT-NOTFOUND + "RC Pass 1 ... no such file"），极易误判成编译器出问题。
CL="$FR_MSVC/bin/Hostx64/x64/cl.exe"
[ -x "$CL" ] || die "cl.exe 不可用：$CL（MSVC 版本目录变了？改 tools/msvc_env.sh 的 _MSVC）"
[ -x "$FR_RC" ] || die "rc.exe 不可用：$FR_RC"
[ -x "$FR_MT" ] || die "mt.exe 不可用：$FR_MT"

# Win32 没有 pthread，pupnp 的 CMakeLists 用 find_package(PTHREADS4W) 找它 —— 必须把
# 预编译安装前缀喂给 CMake，否则配置期直接死在 third_party/pupnp/CMakeLists.txt:228。
# ⚠️ 注意 tools/fetch_deps.sh **不管** pthreads4w（本仓库历史遗留：当年是手工构建到
# third_party/pthreads4w/install 的，构建目录是 build-pthreads/）。所以新机器上先确认这个
# 目录在，否则这里会提前 die 而不是等到 CMake 报一堆看不懂的话。
PTHREADS4W_PREFIX="$ROOT/third_party/pthreads4w/install"
[ -d "$PTHREADS4W_PREFIX" ] || die "缺 $PTHREADS4W_PREFIX —— Windows 构建依赖它（pupnp 需要 PTHREADS4W）。
  tools/fetch_deps.sh 不覆盖此项，需先自行构建 pthreads4w 并安装到该前缀。
  参考：源码 third_party/pthreads4w/，历史构建目录 build-pthreads/"
# ---- Inno Setup（只在要出 exe 安装包时才需要，所以懒加载，不在这里调用）----
# 官方从 6.x 后期起只发签名安装器、不再提供 zip 便携版，于是这里下载后**静默装**到
# dist/.tools/innosetup/：
#   /CURRENTUSER —— 只装给当前用户，不触发 UAC 提权
#   /PORTABLE=1  —— 便携模式，不写系统目录、不留系统级卸载项
# 效果与"解压即用"等价：工具链随 dist/ 一起被 .gitignore 覆盖，换机器自动重下，
# 不污染系统，也不需要管理员权限。
INNO_DIR="$DIST/.tools/innosetup"
ISCC="$INNO_DIR/ISCC.exe"
INNO_SETUP_EXE="$DIST/.tools/innosetup-6.7.3.exe"
INNO_URL="https://github.com/jrsoftware/issrc/releases/download/is-6_7_3/innosetup-6.7.3.exe"
# 官方安装器不带中文包，需另外补（ensure_inno 负责）；放顶层常量是因为 build_exe 也要读它
ZH_LANG="$INNO_DIR/Languages/ChineseSimplified.isl"

ensure_inno() {
  # d188：优先用**现成 ISCC**（CI 用 choco 预装；见 workflow）。
  # 背景：在无桌面会话的 CI runner 里跑 Inno Setup 官方安装器会**挂死**
  #（实测超时后残留 innosetup-6.7.3 / .tmp 孤儿进程），故 CI 侧不跑安装器。
  if [ -n "${FR_ISCC:-}" ] && [ -x "$FR_ISCC" ]; then
    ISCC="$FR_ISCC"
    INNO_DIR="$(dirname "$FR_ISCC")"
    ZH_LANG="$INNO_DIR/Languages/ChineseSimplified.isl"
    log "    ISCC 就位（预装：$ISCC）"
    return 0
  fi
  [ -x "$ISCC" ] && return 0
  log "准备 Inno Setup 6（下载到 dist/.tools/，之后复用）"
  mkdir -p "$DIST/.tools"
  [ -f "$INNO_SETUP_EXE" ] || \
    curl -fsSL --max-time 240 -o "$INNO_SETUP_EXE" "$INNO_URL" \
      || die "Inno Setup 下载失败：$INNO_URL"
  # 安装器是原生 Windows 程序：/DIR 要 "D:\..." 形态（-w，反斜杠）才认
  ( cd "$DIST/.tools" && "./$(basename "$INNO_SETUP_EXE")" /VERYSILENT /SUPPRESSMSGBOXES \
      /NORESTART /NOCANCEL /SP- /CURRENTUSER /PORTABLE=1 /NOICONS \
      /DIR="$(cygpath -w "$INNO_DIR")" ) || die "Inno Setup 静默安装失败（见上面输出）"
  [ -x "$ISCC" ] || die "装完却找不到 $ISCC"
  # 中文语言包：官方安装器不带，要单独补；补不上就退化成英文向导（不让它挡住打包）。
  # 来源用 Inno Setup 官方源码仓库里的那份（标注 6.5.0+，与 6.7.3 相容）。
  if [ ! -f "$ZH_LANG" ]; then
    curl -fsSL --max-time 60 -o "$ZH_LANG" \
      "https://raw.githubusercontent.com/jrsoftware/issrc/main/Files/Languages/ChineseSimplified.isl" \
      || { rm -f "$ZH_LANG"; warn "中文语言包下载失败，安装向导将只有英文"; }
  fi
  log "    ISCC 就位（$( [ -f "$ZH_LANG" ] && echo '含中文向导' || echo '仅英文向导' )）"
}

# ---------------------------------------------------------------- 1. 构建

if [ "$DO_CLEAN" = 1 ]; then
  log "清理 $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

if [ "$SKIP_BUILD" = 0 ]; then
  log "配置（Release + FLASHREC_CONSOLE=OFF —— 发布版必须无控制台窗口）"
  "$FR_CMAKE" -S "$(win_path "$ROOT")" -B "$(win_path "$BUILD_DIR")" -G Ninja \
        -DCMAKE_C_COMPILER="$CL" \
        -DCMAKE_CXX_COMPILER="$CL" \
        -DCMAKE_RC_COMPILER="$(win_path "$FR_RC")" \
        -DCMAKE_MT="$(win_path "$FR_MT")" \
        -DCMAKE_MAKE_PROGRAM="$(win_path "$FR_NINJA")" \
        -DCMAKE_PREFIX_PATH="$(win_path "$PTHREADS4W_PREFIX")" \
        -DCMAKE_BUILD_TYPE=Release \
        -DFLASHREC_CONSOLE=OFF \
        >/dev/null \
    || die "cmake 配置失败（上面是它的原话）"

  log "构建（-j$JOBS）"
  "$FR_NINJA" -C "$(win_path "$BUILD_DIR")" -j "$JOBS" || die "构建失败"
else
  [ -d "$BUILD_DIR" ] || die "--skip-build 但 $BUILD_DIR 不存在"
  log "跳过构建（--skip-build）"
fi

[ -f "$BUILD_DIR/bin/flashrec.exe" ] || die "没找到 $BUILD_DIR/bin/flashrec.exe，构建没成功"

VERSION="$(sed -n 's/^project(FlashRec VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -1 || true)"
[ -n "$VERSION" ] || die "从 CMakeLists.txt 里读不到 project(VERSION)"
log "版本 $VERSION"

# ---------------------------------------------------------------- 2. 暂存（cmake --install）

PKG_NAME="FlashRec-$VERSION-win64"
STAGE="$PKG_WORK/$PKG_NAME"
OUT="$DIST/$PKG_NAME.zip"

rm -rf "$PKG_WORK"
mkdir -p "$STAGE"

log "按 WIN32 安装规则暂存（exe + libmpv-2.dll + assets/）"
"$FR_CMAKE" --install "$(win_path "$BUILD_DIR")" --prefix "$(win_path "$STAGE")" >/dev/null \
  || die "cmake --install 失败"

# ---------------------------------------------------------------- 3. 自检：内容 + 子系统

log "自检 1/2：内容齐备"
missing=""
for f in flashrec.exe libmpv-2.dll assets/default_settings.json \
         assets/scpd/AVTransport.xml assets/scpd/RenderingControl.xml \
         assets/scpd/ConnectionManager.xml assets/icons/app_256.png; do
  [ -e "$STAGE/$f" ] || missing="$missing $f"
done
[ -z "$missing" ] || die "暂存目录缺文件：$missing"

# assets/linux 只该属于 Linux 包（构建期模板），混进 Windows 包是噪音 —— 装上 exclude 后这里应是空
if [ -e "$STAGE/assets/linux" ]; then
  warn "assets/linux 被打进了 Windows 包（CMakeLists.txt 的 WIN32 install 排除规则没生效？）"
fi
log "    内容齐备 ✔"

# ---- 补 VC++ 运行库 ----
# dumpbin /dependents 实测：flashrec.exe 只吃 MSVCP140 / VCRUNTIME140 / VCRUNTIME140_1
# 这三个非系统 dll（libmpv-2.dll 是 MinGW 构建的、不吃 VC 运行库；api-ms-win-crt-* 是
# 系统 UCRT，不该进包）。Win10/11 一般自带，但精简系统 / LTSC 上可能没有 ⇒ 随包带上。
# 刻意**不**捆绑 20MB 的官方 redist 安装器：三个 dll 共约 1MB，放在安装目录既不污染
# 系统、也不会跟别的程序的 redist 打架。
CRT_DIR=""
# FR_MSVC = <VS>/VC/Tools/MSVC/<ver> ⇒ 上溯三级才是 <VS>/VC（redist 挂在 VC/Redist 下）。
# 少一级会落到 VC/Tools，那里根本没有 Redist —— 症状是"静默跳过运行库"。
vc_root="$(cd "$FR_MSVC/../../.." 2>/dev/null && pwd || true)"
if [ -n "$vc_root" ]; then
  # redist 目录名带版本号（14.44.35112），会随 VS 更新变 ⇒ glob 取版本最大的那个
  CRT_DIR="$(ls -1d "$vc_root"/Redist/MSVC/*/x64/Microsoft.VC143.CRT 2>/dev/null | sort -V | tail -1 || true)"
fi
if [ -n "$CRT_DIR" ] && [ -d "$CRT_DIR" ]; then
  ncrt=0
  for dll in msvcp140.dll vcruntime140.dll vcruntime140_1.dll; do
    if [ -f "$CRT_DIR/$dll" ]; then cp -f "$CRT_DIR/$dll" "$STAGE/$dll"; ncrt=$((ncrt+1)); fi
  done
  log "    补 VC++ 运行库 ×$ncrt（$(basename "$(dirname "$CRT_DIR")")）"
else
  warn "找不到 VC++ redist 目录，跳过运行库 —— 目标机若没装 VC++ 2015-2022 运行库会起不来"
fi

# 关键检查：子系统必须是 Windows GUI。CUI 意味着用户双击会先弹控制台窗口。
# FLASHREC_CONSOLE 现默认 OFF，这里仍实测，防止有人把它打开后发布（不能只看参数）。
log "自检 2/2：PE 子系统"
DUMPBIN="$FR_MSVC/bin/Hostx64/x64/dumpbin.exe"
if [ -x "$DUMPBIN" ]; then
  sub="$(  "$DUMPBIN" /headers "$(win_path "$STAGE/flashrec.exe")" 2>/dev/null \
         | grep -iE 'subsystem \(' | head -1 || true)"
  case "$sub" in
    *"Windows GUI"*) log "    $sub ✔（发布版无控制台窗口）" ;;
    "")              warn "    dumpbin 没打印 subsystem 行，跳过该检查" ;;
    *)               die "子系统不是 Windows GUI：$sub
      → FLASHREC_CONSOLE 没关掉，发布出去用户双击会弹黑窗口。检查 -DFLASHREC_CONSOLE=OFF 是否生效。" ;;
  esac
else
  warn "找不到 dumpbin（$DUMPBIN），跳过子系统检查"
fi

# ---------------------------------------------------------------- 4. 压缩

# Git Bash 默认不带 zip 命令，用 python 的 zipfile；实在没有就回退 PowerShell。
zip_with_python() {
  local py="$1"
  "$py" - "$(win_path "$STAGE")" "$(win_path "$OUT")" <<'PY'
import os, sys, zipfile
stage, out = sys.argv[1], sys.argv[2]
stage = os.path.abspath(stage)
# 归档名一律相对 base（= stage 的父目录），这样自然带上顶层目录名 FlashRec-<ver>-win64/，
# 解压不会散落一地。切勿再手工 join 一次顶层名 —— 会套成两层，实测踩过。
base = os.path.dirname(stage)
if os.path.exists(out):
    os.remove(out)
n = 0
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for dp, dns, fns in os.walk(stage):
        dns.sort(); fns.sort()
        for fn in fns:
            full = os.path.join(dp, fn)
            z.write(full, os.path.relpath(full, base).replace(os.sep, "/"))
            n += 1
        # 空目录（assets/fonts 是空的）必须显式写条目，否则解压后不存在
        if not dns and not fns:
            z.writestr(os.path.relpath(dp, base).replace(os.sep, "/") + "/", "")
print(n)
PY
}

# exe 安装包：把 cmake --install 的暂存目录**整体**交给 Inno Setup。
# 文件清单不在 iss 里罗列 —— 暂存目录本身就是 CMakeLists.txt 安装规则的产物，这里再
# 列一份迟早漂移（与上面 ZIP 同一条理由）。
build_exe() {
  ensure_inno
  local iss="$PKG_WORK/installer.iss"
  local exe="$DIST/FlashRec-$VERSION-win64-setup.exe"
  local stage_win dist_win tpl
  stage_win="$(cygpath -w "$STAGE")"
  dist_win="$(cygpath -w "$DIST")"

  log "编译 exe 安装包（Inno Setup 6）"
  # 用 bash 的字符串替换而**不用 sed**：Windows 路径的反斜杠进 sed 替换段会被当转义
  # 吃掉（\d → d），${var//pat/rep} 对反斜杠是字面的，安全。
  tpl="$(cat "$ROOT/tools/installer_windows.iss")"
  tpl="${tpl//__FR_VERSION__/$VERSION}"
  tpl="${tpl//__FR_SOURCE__/$stage_win}"
  tpl="${tpl//__FR_OUTPUT__/$dist_win}"
  printf '%s\n' "$tpl" > "$iss"
  if grep -q '__FR_' "$iss"; then die "installer.iss 里还有没替换掉的占位符"; fi

  rm -f "$exe"
  # 有中文语言包就编进来（/DFR_ZH 打开 installer_windows.iss 里的中文段），没有就英文
  local zh_flag=()
  if [ -f "$ZH_LANG" ]; then zh_flag=(/DFR_ZH); fi
  # ISCC 是原生 Windows 程序：脚本路径给 -w（反斜杠）形态。
  # ⚠️ 必须关掉 MSYS 的**参数路径转换**：否则以 `/` 开头的 `/DFR_ZH` 会被改写成
  # `C:/Program Files/Git/DFR_ZH`，ISCC 把它当成**第二个脚本名** → 报
  # "You may not specify more than one script filename"（本机实测踩过）。
  # 脚本路径本身已是 Windows 反斜杠绝对路径，无需转换，故整条命令关掉转换最稳。
  MSYS2_ARG_CONV_EXCL='*' MSYS_NO_PATHCONV=1 \
    "$ISCC" "${zh_flag[@]}" "$(cygpath -w "$iss")" || die "ISCC 编译失败（上面是它的原话）"
  [ -f "$exe" ] || die "ISCC 没产出 $exe"
  log "→ $exe（$(du -h "$exe" | cut -f1)）"
}

# ---------------------------------------------------------------- 4. 便携 ZIP

if [ "$ONLY" != "exe" ]; then
log "压缩 → $OUT"
mkdir -p "$DIST"
rm -f "$OUT"

PY=""
for cand in python python3 py; do
  if command -v "$cand" >/dev/null 2>&1; then PY="$cand"; break; fi
done

if [ -n "$PY" ]; then
  nfiles="$(zip_with_python "$PY")" || die "python 压缩失败"
  log "    $PY 压缩完成，写入 $nfiles 个文件"
else
  # 回退：Windows 自带的 tar.exe 是 bsdtar（libarchive），带 -a 能按扩展名直接写 zip。
  # 刻意不用 PowerShell 的 Compress-Archive —— 它在本项目常驻的工具环境里会被安全策略
  # 拦下（"Invoking PowerShell from Bash bypasses PowerShell security checks"），
  # 也就是说那条回退路径在生产环境里根本跑不通，写它等于没写。
  warn "没找到 python，回退 Windows 自带 bsdtar（116MB 的 dll 会比较慢）"
  TAR_BIN="/c/Windows/System32/tar.exe"
  [ -x "$TAR_BIN" ] || die "既没有 python 也没有 $TAR_BIN，无法压缩"
  "$TAR_BIN" -a -c -f "$(win_path "$OUT")" -C "$(win_path "$PKG_WORK")" "$PKG_NAME" \
    || die "bsdtar 压缩失败"
fi

[ -f "$OUT" ] || die "没产出 ZIP"
fi

# ---------------------------------------------------------------- 5. exe 安装包

if [ "$ONLY" != "zip" ]; then
  build_exe
fi

# ---------------------------------------------------------------- 6. 冒烟（可选）

if [ "$DO_SMOKE" = 1 ]; then
  log "冒烟：限时启动 6s（应用是单实例的，先关掉别的实例才准；期间托盘会出现一下）"
  # ⚠️ 若在自动化/agent 宿主里跑，宿主可能在命令结束时回收后台子进程，导致这里数到 0 ——
  # 那是环境行为不是崩溃（判定崩溃请看有无新 .dmp，或看 exe 的 stdout：正常启动会打到
  # "mpv render context 就绪"）。真实终端里不会有这个问题。
  ( cd "$STAGE" && ./flashrec.exe >/dev/null 2>&1 & ) || true
  sleep 6
  # 用 tasklist 数进程，不用 PowerShell —— 见上面压缩回退里的同一条理由。
  # `//FI` 的双斜杠是 Git Bash 的转义：单斜杠会被 MSYS 当路径转换吃掉。
  alive="$(tasklist //FI "IMAGENAME eq flashrec.exe" //NH 2>/dev/null \
           | grep -c 'flashrec\.exe' || true)"
  if [ "$alive" = "1" ]; then
    log "    启动后 6s 仍在运行 ⇒ 没崩 ✔；现在用 quit_app.py 请它优雅退出"
    python "$ROOT/tools/quit_app.py" >/dev/null 2>&1 || warn "    quit_app.py 未成功，可能还有实例在跑"
  elif [ "$alive" = "0" ]; then
    warn "    启动后没找到进程 —— 可能起不来，或已被单实例逻辑挡下（先关掉别的实例再试）"
  else
    warn "    进程数读取失败（得到 '$alive'），跳过冒烟判定"
  fi
fi

# ---------------------------------------------------------------- 汇总

echo >&2
log "完成。dist/ 发布会话："
shopt -s nullglob
for f in "$DIST"/*.deb "$DIST"/*.AppImage "$DIST"/*.zip "$DIST"/*.exe; do
  printf '    %-8s %s\n' "$(du -h "$f" | cut -f1)" "$(basename "$f")" >&2
done
