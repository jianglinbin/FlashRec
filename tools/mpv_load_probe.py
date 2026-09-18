#!/usr/bin/env python3
"""脱离 FlashRec 进程，直接用随包的 libmpv 加载一个 URL/文件并打印 mpv 原始日志。

用途：当 FlashRec 报 `mpv error → STOPPED` 时，用来区分
  (a) mpv/ffmpeg 本身打不开这个源（源的问题 / 编码不支持）
  (b) FlashRec 的接线问题（渲染上下文、状态机、loadfile 参数）
`vo=null`/`ao=null` 把渲染与音频输出摘掉，只留解复用+解码。

用法：
  python tools/mpv_load_probe.py <url-or-path> [--hwdec auto-safe] [--secs 8]
"""
from __future__ import annotations

import argparse
import ctypes
import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "bin"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("target")
    ap.add_argument("--hwdec", default="auto-safe")
    ap.add_argument("--secs", type=float, default=8.0)
    ap.add_argument("--libdir", default=str(BIN))
    args = ap.parse_args()

    libdir = Path(args.libdir)
    dll = libdir / "libmpv-2.dll"
    if not dll.exists():
        print(f"找不到 {dll}")
        return 2
    os.add_dll_directory(str(libdir))
    lib = ctypes.CDLL(str(dll))
    lib.mpv_create.restype = ctypes.c_void_p
    lib.mpv_set_option_string.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.mpv_initialize.argtypes = [ctypes.c_void_p]
    lib.mpv_command.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p)]
    lib.mpv_get_property.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p]
    lib.mpv_get_property_string.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.mpv_get_property_string.restype = ctypes.c_void_p
    lib.mpv_free.argtypes = [ctypes.c_void_p]
    lib.mpv_terminate_destroy.argtypes = [ctypes.c_void_p]
    lib.mpv_error_string.argtypes = [ctypes.c_int]
    lib.mpv_error_string.restype = ctypes.c_char_p

    h = lib.mpv_create()
    if not h:
        print("mpv_create 失败")
        return 2

    def opt(k: str, v: str) -> None:
        lib.mpv_set_option_string(h, k.encode(), v.encode())

    opt("terminal", "yes")          # mpv 日志直接打到 stderr
    opt("msg-level", "all=v")       # 最详细
    opt("vo", "null")               # 摘掉视频输出
    opt("ao", "null")               # 摘掉音频输出
    opt("hwdec", args.hwdec)
    opt("idle", "yes")
    opt("demuxer-max-bytes", "64MiB")
    if lib.mpv_initialize(h) < 0:
        print("mpv_initialize 失败")
        return 2

    def gstr(name: str) -> str:
        p = lib.mpv_get_property_string(h, name.encode())
        if not p:
            return "<fail>"
        s = ctypes.cast(p, ctypes.c_char_p).value.decode("utf-8", "ignore")
        lib.mpv_free(p)
        return s

    def gnum(name: str) -> float:
        d = ctypes.c_double(-1.0)
        lib.mpv_get_property(h, name.encode(), 5, ctypes.byref(d))
        return d.value

    target = args.target
    print(f"===== libmpv 直载 {target}  (hwdec={args.hwdec}, vo=null) =====", flush=True)
    rc = lib.mpv_command(h, (ctypes.c_char_p * 4)(b"loadfile", target.encode(), b"replace", None))
    print(f"loadfile rc={rc} ({lib.mpv_error_string(rc).decode() if rc < 0 else 'ok'})", flush=True)

    t0 = time.time()
    pos = -1.0
    while time.time() - t0 < args.secs:
        time.sleep(0.3)
        pos = gnum("time-pos")
        sa = gstr("seeking")
        idle = gstr("idle-active")
        if pos > 1.0:
            break
        if idle == "yes" and time.time() - t0 > 2.0:
            break

    print("\n--- 观测结果 ---", flush=True)
    print(f"  time-pos      = {pos:.3f}", flush=True)
    print(f"  duration      = {gnum('duration'):.3f}", flush=True)
    print(f"  seekable      = {gstr('seekable')}", flush=True)
    print(f"  idle-active   = {gstr('idle-active')}", flush=True)
    print(f"  video-codec   = {gstr('video-codec')}", flush=True)
    print(f"  video-format  = {gstr('video-format')}", flush=True)
    print(f"  video-bitrate = {gnum('video-bitrate'):.0f}", flush=True)
    print(f"  hwdec-current = {gstr('hwdec-current')}", flush=True)
    print(f"  audio-codec   = {gstr('audio-codec')}", flush=True)
    print(f"  audio-bitrate = {gnum('audio-bitrate'):.0f}", flush=True)
    print(f"  audio-sr      = {gnum('audio-params/samplerate'):.0f}", flush=True)
    print(f"  audio-ch      = {gnum('audio-params/channel-count'):.0f}", flush=True)
    print(f"  path          = {gstr('path')}", flush=True)
    print("  判定：" + ("成功（位置已推进，mpv 本身能播）" if pos > 1.0 else "失败（mpv 本身播不了这个源）"), flush=True)
    lib.mpv_terminate_destroy(h)
    return 0 if pos > 1.0 else 1


if __name__ == "__main__":
    sys.exit(main())
