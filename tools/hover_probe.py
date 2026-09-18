#!/usr/bin/env python3
"""把光标移到 FlashRec 窗口的指定位置并按需发送 WM_MOUSEMOVE，用于唤醒自动隐藏的
chrome（顶栏/底栏）或验证悬停态渲染。

背景：播放态下 chrome_fade() 会在 idleHideSec（默认 2s）无操作后把两栏淡出到 0，
这是设计行为。想核对"按钮是否画得出来"，必须先把光标移进窗口内对应区域。

只做两件外部可见的事：SetCursorPos + PostMessage(WM_MOUSEMOVE)。不点击、不改窗口状态。

用法:
  python tools/hover_probe.py                 # 移到底栏右侧（全屏键附近）
  python tools/hover_probe.py bottom-left     # 底栏左侧（播放/上一集/下一集）
  python tools/hover_probe.py bottom-center   # 底栏中部（进度条）
  python tools/hover_probe.py top-right       # 顶栏三键
  python tools/hover_probe.py center          # 窗口中央
  python tools/hover_probe.py --list          # 只打印窗口几何，不移动
"""
import ctypes
import sys
import time
from ctypes import wintypes

# 必须在任何窗口 API 之前：FlashRec 是 per-monitor DPI aware，客户区是物理像素。
# 本脚本若保持 DPI UNAWARE，读到的尺寸会被 Windows 虚拟化缩小（125% 下 ×0.8），
# 于是算出的光标落点会偏到窗口外/错位，hover 测不准。
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)  # PER_MONITOR_DPI_AWARE
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

user32 = ctypes.windll.user32

WM_MOUSEMOVE = 0x0200

TITLE = "FlashRec 投屏接收"


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def find_window():
    hwnd = user32.FindWindowW(None, TITLE)
    if hwnd:
        return hwnd
    # 退路：按标题前缀找（FriendlyName 可能被用户改过）
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, _l):
        n = user32.GetWindowTextLengthW(h)
        if n > 0:
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(h, buf, n + 1)
            if "FlashRec" in buf.value:
                found.append(h)
        return True

    user32.EnumWindows(cb, 0)
    return found[0] if found else 0


def main():
    targets = {
        "bottom-right": lambda w, h: (w - 60, h - 22),
        "bottom-left": lambda w, h: (96, h - 22),
        "bottom-center": lambda w, h: (w // 2, h - 22),
        "top-right": lambda w, h: (w - 40, 17),
        "center": lambda w, h: (w // 2, h // 2),
    }
    which = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") \
        else "bottom-right"

    hwnd = find_window()
    if not hwnd:
        print("未找到 FlashRec 窗口（app 未运行？）")
        return 1

    cr = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(cr))
    w, h = cr.right - cr.left, cr.bottom - cr.top
    wr = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(wr))
    print(f"hwnd=0x{hwnd:X} client={w}x{h} window={wr.right - wr.left}x{wr.bottom - wr.top}")

    if "--list" in sys.argv:
        for k in targets:
            print(f"  {k:15s} -> client {targets[k](w, h)}")
        return 0

    fn = targets.get(which)
    if fn is None:
        print(f"未知位置 {which!r}，可选: {', '.join(targets)}")
        return 2

    cx, cy = fn(w, h)
    p = POINT(cx, cy)
    user32.ClientToScreen(hwnd, ctypes.byref(p))
    # 先抢前台：非前台窗口收不到系统的 WM_MOUSEMOVE（GLFW 依赖它派发鼠标回调），
    # 只发 PostMessage 会被忽略 —— 于是"移光标"看起来毫无效果。
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.05)
    user32.SetCursorPos(p.x, p.y)
    # 连发几次，保证被事件循环采到（主线程每帧读一次光标）
    lparam = ((p.y & 0xFFFF) << 16) | (p.x & 0xFFFF)
    for _ in range(6):
        user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, lparam)
        time.sleep(0.06)
    print(f"光标已移到 {which}：client=({cx},{cy}) screen=({p.x},{p.y}) "
          f"前台={user32.GetForegroundWindow() == hwnd}")
    time.sleep(0.3)
    return 0


if __name__ == "__main__":
    sys.exit(main())
