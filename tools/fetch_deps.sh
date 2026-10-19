#!/usr/bin/env bash
# 第三方依赖预取：把依赖预克隆到 third_party/（CMake 检测到就用，缺失时 FetchContent 兜底），
# 并下载 libmpv 预编译开发包、离线生成 glad GL loader。
# 用法: bash tools/fetch_deps.sh   （可重复执行，已存在自动跳过；日志 third_party/_fetch.log）
set -u
cd "$(dirname "$0")/.."
mkdir -p third_party
LOG=third_party/_fetch.log
: > "$LOG"

clone() {
  local dir=$1 url=$2 tag=$3
  if [ -d "third_party/$dir/.git" ]; then echo "[skip] $dir 已存在" | tee -a "$LOG"; return 0; fi
  local args=(--depth 1 --single-branch)
  [ -n "$tag" ] && args+=(--branch "$tag")
  echo "[clone] $dir <- $url (tag:$tag)" | tee -a "$LOG"
  if git clone "${args[@]}" "$url" "third_party/$dir" >>"$LOG" 2>&1; then
    echo "[ok] $dir" | tee -a "$LOG"
  else
    rm -rf "third_party/$dir"
    echo "[FAIL] $dir" | tee -a "$LOG"
  fi
}

clone glfw   https://github.com/glfw/glfw.git        3.4 &
clone nanovg https://github.com/memononen/nanovg.git "" &
clone spdlog https://github.com/gabime/spdlog.git    v1.14.1 &
clone pupnp  https://github.com/pupnp/pupnp.git      branch-1.14.x &
clone curl   https://github.com/curl/curl.git        curl-8_23_0 &
wait

# ---- UxPlay（v0.6.0 AirPlay 线 / d336）：pin 57ea834 ----
# pin 是 SHA 不是 tag，clone() 的 --depth 1 --branch 装不下 SHA ⇒ 专用全量 clone（仓库小）+ detach。
if [ -d third_party/uxplay/.git ]; then
  echo "[skip] uxplay 已存在" | tee -a "$LOG"
else
  echo "[clone] uxplay <- https://github.com/FDH2/UxPlay.git (pin:57ea834)" | tee -a "$LOG"
  if git clone -q https://github.com/FDH2/UxPlay.git third_party/uxplay >>"$LOG" 2>&1 \
     && git -C third_party/uxplay checkout -q --detach 57ea834 >>"$LOG" 2>&1; then
    echo "[ok] uxplay" | tee -a "$LOG"
  else
    rm -rf third_party/uxplay
    echo "[FAIL] uxplay" | tee -a "$LOG"
  fi
fi

# ---- OpenSSL/libplist（v0.6.0 / d336）----
# uxplay.cmake 前缀约定：third_party/vcpkg/installed/x64-windows（Windows）。
# CI/本机均优先复用现成 vcpkg（PATH 或 VCPKG_INSTALLATION_ROOT），装完把
# installed/x64-windows 拷进约定前缀；本机已有 third_party/vcpkg 则自动跳过。
# Linux 走系统包（libssl-dev / libplist-dev），由 workflow「Install build deps」负责。
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    # uxplay.cmake 需要两处：installed 前缀（openssl/libplist）+ buildtrees 里的
    # openssl ms/applink.c（OPENSSL_Applink，缺了启动即崩，见 uxplay.cmake:305-315）。
    APPLINK_LOCAL="$(ls third_party/vcpkg/buildtrees/openssl/src/*/ms/applink.c 2>/dev/null | head -1)"
    if [ -f third_party/vcpkg/installed/x64-windows/include/openssl/ssl.h ] \
       && [ -f third_party/vcpkg/installed/x64-windows/lib/plist-2.0.lib ] \
       && [ -n "$APPLINK_LOCAL" ]; then
      echo "[skip] vcpkg openssl+libplist+applink 已存在" | tee -a "$LOG"
    else
      VK="$(command -v vcpkg || true)"
      [ -z "$VK" ] && [ -n "${VCPKG_INSTALLATION_ROOT:-}" ] && VK="$VCPKG_INSTALLATION_ROOT/vcpkg"
      if [ -z "$VK" ] || [ ! -x "$VK" ]; then
        echo "[FAIL] 找不到 vcpkg（PATH 或 VCPKG_INSTALLATION_ROOT），无法装 openssl/libplist" | tee -a "$LOG"
      else
        VROOT="$(cd "$(dirname "$VK")" && pwd)"
        echo "[vcpkg] $VK install openssl libplist (x64-windows)" | tee -a "$LOG"
        "$VK" install openssl:x64-windows libplist:x64-windows --no-print-usage >>"$LOG" 2>&1
        SRCI="$VROOT/installed/x64-windows"
        if [ -f "$SRCI/include/openssl/ssl.h" ] && [ -f "$SRCI/lib/plist-2.0.lib" ]; then
          mkdir -p third_party/vcpkg/installed/x64-windows
          cp -r "$SRCI/include" "$SRCI/lib" "$SRCI/bin" third_party/vcpkg/installed/x64-windows/ 2>>"$LOG"
          # applink.c：从 vcpkg 的 openssl buildtrees 源码树取（uxplay.cmake 按该路径 glob）
          APPLINK_SRC="$(ls "$VROOT"/buildtrees/openssl/src/*/ms/applink.c 2>/dev/null | head -1)"
          if [ -n "$APPLINK_SRC" ]; then
            mkdir -p third_party/vcpkg/buildtrees/openssl/src/fr-fetch-shim/ms
            cp "$APPLINK_SRC" third_party/vcpkg/buildtrees/openssl/src/fr-fetch-shim/ms/applink.c
          fi
          if [ -n "$(ls third_party/vcpkg/buildtrees/openssl/src/*/ms/applink.c 2>/dev/null | head -1)" ]; then
            echo "[ok] vcpkg openssl+libplist+applink" | tee -a "$LOG"
          else
            echo "[FAIL] installed 已拷但 applink.c 缺（buildtrees 被清理？），日志尾部：" | tee -a "$LOG"; tail -25 "$LOG"
          fi
        else
          echo "[FAIL] vcpkg 装完仍缺 ssl.h/plist-2.0.lib，日志尾部：" | tee -a "$LOG"; tail -25 "$LOG"
        fi
      fi
    fi
    ;;
  *)
    if pkg-config --exists libplist-2.0 2>/dev/null && pkg-config --exists openssl 2>/dev/null; then
      echo "[ok] 系统 openssl+libplist 在位" | tee -a "$LOG"
    else
      echo "[FAIL] 缺 libplist-dev / libssl-dev（Linux 由 workflow apt 装）" | tee -a "$LOG"
    fi
    ;;
esac

# glad：GL 3.3 核心加载器（pip 装生成器后离线生成）
# d187：python 不再写死 —— 优先 FR_PYTHON，其次 PATH 上的 python/python3（CI 友好）。
PY="${FR_PYTHON:-$(command -v python || command -v python3 || true)}"
export PYTHONIOENCODING=utf-8  # d188：避免 CI 非 UTF-8 控制台打印中文崩溃
# 默认走官方 PyPI（CI 在境外；本机可用 FR_PIP_INDEX 覆盖为清华镜像）
PIP_INDEX="${FR_PIP_INDEX:-https://pypi.org/simple}"
if [ -f "third_party/glad/include/glad/gl.h" ]; then
  echo "[skip] glad 已存在" | tee -a "$LOG"
elif [ -z "$PY" ]; then
  echo "[FAIL] 找不到 python（可设 FR_PYTHON）" | tee -a "$LOG"
else
  VENV="${FR_VENV:-third_party/.pyvenv}"
  VP="$VENV/Scripts/python.exe"; [ -x "$VP" ] || VP="$VENV/bin/python"
  if [ ! -x "$VP" ]; then
    "$PY" -m venv "$VENV" >>"$LOG" 2>&1 && { VP="$VENV/Scripts/python.exe"; [ -x "$VP" ] || VP="$VENV/bin/python"; }
  fi
  [ -x "$VP" ] || VP="$PY"
  if "$VP" -m pip install -q --disable-pip-version-check -i "$PIP_INDEX" glad2 >>"$LOG" 2>&1 \
     || "$VP" -m pip install -q --disable-pip-version-check glad2 >>"$LOG" 2>&1; then
    # glad2 用**子命令** `c`（不是 glad1 的 --generator c）；两种语法都试，兼容新旧。
    if "$VP" -m glad --api 'gl:core=3.3' --out-path third_party/glad c >>"$LOG" 2>&1 \
       || "$VP" -m glad --generator c --api 'gl:core=3.3' --out-path third_party/glad >>"$LOG" 2>&1; then
      echo "[ok] glad" | tee -a "$LOG"
    else
      echo "[FAIL] glad 生成失败，日志尾部：" | tee -a "$LOG"; tail -25 "$LOG"
    fi
  else
    echo "[FAIL] glad2 安装失败，日志尾部：" | tee -a "$LOG"; tail -20 "$LOG"
  fi
fi

# mpv-dev（libmpv 预编译开发包，含 import lib 与 dll）—— **仅 Windows 需要**；
# Linux 用系统 libmpv（apt libmpv-dev），此处跳过，避免无谓的 FAIL 噪音。
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    if [ -z "$PY" ]; then
      echo "[FAIL] 找不到 python，无法下载 mpv-dev（可设 FR_PYTHON）" | tee -a "$LOG"
      exit 1
    fi
    if "$PY" tools/fetch_mpv.py >>"$LOG" 2>&1; then
      echo "[ok] mpv-dev" | tee -a "$LOG"
    else
      # mpv 是硬依赖：这里失败必须让 CI 停在本步，不能拖到 CMake 配置才暴露。
      echo "[FAIL] mpv-dev，日志尾部：" | tee -a "$LOG"; tail -25 "$LOG"
      exit 1
    fi
    ;;
  *) echo "[skip] mpv-dev（非 Windows，用系统 libmpv）" | tee -a "$LOG" ;;
esac

echo "done" | tee -a "$LOG"
