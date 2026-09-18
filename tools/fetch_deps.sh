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

# glad：GL 3.3 核心加载器（pip 装生成器后离线生成，走清华镜像）
if [ ! -f "third_party/glad/include/glad/gl.h" ]; then
  PY_MGR="/c/Users/ben/.workbuddy/binaries/python/versions/3.13.12/python.exe"
  VENV="/c/Users/ben/.workbuddy/binaries/python/envs/default"
  if [ ! -x "$VENV/Scripts/python.exe" ]; then
    "$PY_MGR" -m venv "$VENV" >>"$LOG" 2>&1
  fi
  if "$VENV/Scripts/python.exe" -m pip install -q -i https://pypi.tsunghua.tsinghua.edu.cn/simple glad2 >>"$LOG" 2>&1 \
     || "$VENV/Scripts/python.exe" -m pip install -q -i https://pypi.tuna.tsinghua.edu.cn/simple glad2 >>"$LOG" 2>&1; then
    "$VENV/Scripts/python.exe" -m glad --generator c --api 'gl:core=3.3' --out-path third_party/glad >>"$LOG" 2>&1 \
      && echo "[ok] glad" | tee -a "$LOG" || echo "[FAIL] glad" | tee -a "$LOG"
  else
    echo "[FAIL] glad 生成器安装" | tee -a "$LOG"
  fi
else
  echo "[skip] glad 已存在" | tee -a "$LOG"
fi

# mpv-dev（libmpv 预编译开发包，含 import lib 与 dll）
"C:/Users/ben/.workbuddy/binaries/python/versions/3.13.12/python.exe" tools/fetch_mpv.py >>"$LOG" 2>&1 \
  && echo "[ok] mpv-dev" | tee -a "$LOG" || echo "[FAIL] mpv-dev（详见 third_party/_fetch.log）" | tee -a "$LOG"

echo "done" | tee -a "$LOG"
