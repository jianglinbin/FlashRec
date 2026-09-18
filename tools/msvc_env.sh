#!/usr/bin/env bash
# MSVC + SDK 构建环境（Git Bash 内 source 本文件后可用 cl/link/ninja/cmake）。
# 用法: source tools/msvc_env.sh && ninja -C build
_MSVC="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207"
export FR_SDK="C:/Program Files (x86)/Windows Kits/10"
export FR_SDKVER=10.0.26100.0
export FR_BT="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake"
export FR_CMAKE="$FR_BT/CMake/bin/cmake.exe"
export FR_NINJA="$FR_BT/Ninja/ninja.exe"
export FR_MSVC="$_MSVC"
# SDK 侧的两个工具：rc.exe（资源编译器）与 mt.exe（清单工具）。
# CMake 在链接期会去调它们 —— 找不到就报 `CMAKE_MT-NOTFOUND` / `rc ... no such file`，
# 症状是"编译过了、链接挂"，很容易误判成编译器有问题。这里导出成变量，供显式 -D 使用。
export FR_RC="$FR_SDK/bin/$FR_SDKVER/x64/rc.exe"
export FR_MT="$FR_SDK/bin/$FR_SDKVER/x64/mt.exe"

export INCLUDE="$_MSVC/include;$FR_SDK/Include/$FR_SDKVER/ucrt;$FR_SDK/Include/$FR_SDKVER/shared;$FR_SDK/Include/$FR_SDKVER/um;$FR_SDK/Include/$FR_SDKVER/winrt;$FR_SDK/Include/$FR_SDKVER/cppwinrt"
export LIB="$_MSVC/lib/x64;$FR_SDK/Lib/$FR_SDKVER/ucrt/x64;$FR_SDK/Lib/$FR_SDKVER/um/x64"
export LIBPATH="$LIB"

# ⚠️ PATH 必须转成 MSYS 形态（/c/...）再拼接 —— 直接把 "C:/..." 塞进 bash 的 PATH 是**错的**：
# bash 以 ':' 分隔 PATH，而 "C:/Program Files (x86)/..." 自带一个冒号，会被切成 "C" 和
# "/Program Files (x86)/..." 两条（`echo $PATH | tr ':' '\n' | head -3` 一看便知）。
# 后果是 cl / rc / mt / ninja / dumpbin **全都查不到**，而且失败得很隐蔽：
#   · CMake 找不到编译器 → "CMAKE_C_COMPILER not set, after EnableLanguage"
#   · 找到编译器但找不到 rc/mt → "编译过了、链接挂"（RC Pass 1 ... no such file）
# 历史上 build/ 是绕开 PATH、把 CMAKE_C_COMPILER / CXX_COMPILER / RC_COMPILER / MT 逐个显式
# -D 传进去配出来的，所以这个 bug 一直没暴露。用 cygpath -u 转换后 PATH 才真正可用。
if command -v cygpath >/dev/null 2>&1; then
  _msys() { cygpath -u "$1"; }
else
  _msys() { printf '%s' "$1"; }   # 非 MSYS 环境（不该出现）：原样返回，至少不更坏
fi
export PATH="$(_msys "$_MSVC")/bin/Hostx64/x64:$(_msys "$FR_SDK")/bin/$FR_SDKVER/x64:$(_msys "$FR_BT")/CMake/bin:$(_msys "$FR_BT")/Ninja:$PATH"
unset -f _msys
