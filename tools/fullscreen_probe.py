#!/usr/bin/env python3
"""触发/查询 FlashRec 全屏，并打印全屏前后的三组尺寸（外框/客户区/DPI）。

只改本窗口的全屏状态，不改播放内容。用法:
  python tools/fullscreen_probe.py toggle
  python tools/fullscreen_probe.py info
"""
import ctypes
import sys
import time
from ctypes import wintypes

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

u = ctypes.windll.user32
TITLE = "FlashRec 投屏接收"
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
VK_RETURN = 0x0D
VK_CONTROL = 0x11


def find():
    h = u.FindWindowW(None, TITLE)
    if h:
        return h
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hw, _l):
        n = u.GetWindowTextLengthW(hw)
        if n:
            b = ctypes.create_unicode_buffer(n + 1)
            u.GetWindowTextW(hw, b, n + 1)
            if "FlashRec" in b.value:
                found.append(hw)
        return True

    u.EnumWindows(cb, 0)
    return found[0] if found else 0


def info(tag):
    h = find()
    if not h:
        print("未找到窗口")
        return
    cr = wintypes.RECT(); u.GetClientRect(h, ctypes.byref(cr))
    wr = wintypes.RECT(); u.GetWindowRect(h, ctypes.byref(wr))
    mon = u.MonitorFromWindow(h, 2)  # NEAREST
    dpi = u.GetDpiForWindow(h) if hasattr(u, "GetDpiForWindow") else 96
    class MONITORINFO(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT),
                    ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD)]
    mi = MONITORINFO(); mi.cbSize = ctypes.sizeof(MONITORINFO)
    u.GetMonitorInfoW(mon, ctypes.byref(mi))
    mw = mi.rcMonitor.right - mi.rcMonitor.left
    mh = mi.rcMonitor.bottom - mi.rcMonitor.top
    print(f"[{tag}] 客户区={cr.right - cr.left}x{cr.bottom - cr.top} "
          f"外框={wr.right - wr.left}x{wr.bottom - wr.top} "
          f"位置=({wr.left},{wr.top}) 显示器={mw}x{mh} dpi={dpi}")
    if cr.right - cr.left == mw and cr.bottom - cr.top == mh:
        print("      → 客户区 == 显示器尺寸，全屏尺寸正确 ✓")
    else:
        print(f"      → 与显示器差 {mw - (cr.right - cr.left)}x{mh - (cr.bottom - cr.top)}")


def main():
    if len(sys.argv) < 2 or sys.argv[1] == "info":
        info("info")
        return 0
    h = find()
    if not h:
        print("未找到窗口")
        return 1
    u.SetForegroundWindow(h)
    time.sleep(0.1)
    info("全屏前")
    # 应用内全屏快捷键 = Ctrl+Enter（见 main.cpp on_key）
    u.keybd_event(VK_CONTROL, 0, 0, 0)
    u.PostMessageW(h, WM_KEYDOWN, VK_RETURN, 0)
    u.PostMessageW(h, WM_KEYUP, VK_RETURN, 0)
    u.keybd_event(VK_CONTROL, 0, 2, 0)
    time.sleep(1.0)
    info("全屏后")
    return 0


if __name__ == "__main__":
    sys.exit(main())
