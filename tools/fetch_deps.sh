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
wait

# glad：GL 3.3 核心加载器（pip 装生成器后离线生成）
# d187：python 不再写死 —— 优先 FR_PYTHON，其次 PATH 上的 python/python3（CI 友好）。
PY="${FR_PYTHON:-$(command -v python || command -v python3 || true)}"
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
    if "$VP" -m glad --generator c --api 'gl:core=3.3' --out-path third_party/glad >>"$LOG" 2>&1; then
      echo "[ok] glad" | tee -a "$LOG"
    else
      echo "[FAIL] glad 生成失败，日志尾部：" | tee -a "$LOG"; tail -20 "$LOG"
    fi
  else
    echo "[FAIL] glad2 安装失败，日志尾部：" | tee -a "$LOG"; tail -20 "$LOG"
  fi
fi

# mpv-dev（libmpv 预编译开发包，含 import lib 与 dll）—— **仅 Windows 需要**；
# Linux 用系统 libmpv（apt libmpv-dev），此处跳过，避免无谓的 FAIL 噪音。
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    if [ -n "$PY" ]; then
      "$PY" tools/fetch_mpv.py >>"$LOG" 2>&1 \
        && echo "[ok] mpv-dev" | tee -a "$LOG" || echo "[FAIL] mpv-dev（详见 third_party/_fetch.log）" | tee -a "$LOG"
    fi
    ;;
  *) echo "[skip] mpv-dev（非 Windows，用系统 libmpv）" | tee -a "$LOG" ;;
esac

echo "done" | tee -a "$LOG"
