#!/usr/bin/env bash
# MSVC + SDK 构建环境（Git Bash 内 source 本文件后可用 cl/link/ninja/cmake）。
# 用法: source tools/msvc_env.sh && ninja -C build
#
# d187：支持**外部覆盖**。CI（GitHub Actions）先用自己的方式定位 VS/SDK 并导出
# FR_CMAKE / FR_NINJA / FR_MSVC / FR_SDK / FR_SDKVER / FR_BT / FR_RC / FR_MT，
# 本文件只在**未设置**时填入本机默认值 —— 这样本机（VS BuildTools）与 CI（VS Enterprise）
# 各用各的路径，不必改脚本。
_MSVC_DEFAULT="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207"
_FR_BT_DEFAULT="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake"
export FR_SDK="${FR_SDK:-C:/Program Files (x86)/Windows Kits/10}"
export FR_SDKVER="${FR_SDKVER:-10.0.26100.0}"
export FR_BT="${FR_BT:-$_FR_BT_DEFAULT}"
export FR_CMAKE="${FR_CMAKE:-$FR_BT/CMake/bin/cmake.exe}"
export FR_NINJA="${FR_NINJA:-$FR_BT/Ninja/ninja.exe}"
export FR_MSVC="${FR_MSVC:-$_MSVC_DEFAULT}"
_MSVC="$FR_MSVC"
# SDK 侧的两个工具：rc.exe 与 mt.exe（缺则"编译过了、链接挂"，症状隐蔽）。
export FR_RC="${FR_RC:-$FR_SDK/bin/$FR_SDKVER/x64/rc.exe}"
export FR_MT="${FR_MT:-$FR_SDK/bin/$FR_SDKVER/x64/mt.exe}"

export INCLUDE="${INCLUDE:-$_MSVC/include;$FR_SDK/Include/$FR_SDKVER/ucrt;$FR_SDK/Include/$FR_SDKVER/shared;$FR_SDK/Include/$FR_SDKVER/um;$FR_SDK/Include/$FR_SDKVER/winrt;$FR_SDK/Include/$FR_SDKVER/cppwinrt}"
export LIB="${LIB:-$_MSVC/lib/x64;$FR_SDK/Lib/$FR_SDKVER/ucrt/x64;$FR_SDK/Lib/$FR_SDKVER/um/x64}"
export LIBPATH="$LIB"

# ⚠️ PATH 必须转成 MSYS 形态（/c/...）再拼接 —— 直接把 "C:/..." 塞进 bash 的 PATH 是**错的**：
# bash 以 ':' 分隔 PATH，而 "C:/Program Files (x86)/..." 自带一个冒号，会被切开。
if command -v cygpath >/dev/null 2>&1; then
  _msys() { cygpath -u "$1"; }
else
  _msys() { printf '%s' "$1"; }
fi
export PATH="$(_msys "$_MSVC")/bin/Hostx64/x64:$(_msys "$FR_SDK")/bin/$FR_SDKVER/x64:$(_msys "$FR_BT")/CMake/bin:$(_msys "$FR_BT")/Ninja:$PATH"
unset -f _msys
