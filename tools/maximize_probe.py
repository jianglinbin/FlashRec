#!/usr/bin/env python3
"""触发/还原 FlashRec 最大化，并打印最大化前后的四组尺寸（外框矩形/客户区矩形/工作区/DPI）。

只改本窗口的最大化状态，不改播放内容。用法:
  python tools/maximize_probe.py toggle
  python tools/maximize_probe.py info
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
WM_SYSCOMMAND = 0x0112
SC_MAXIMIZE = 0xF030
SC_RESTORE = 0xF120


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


class MONITORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT),
                ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD)]


def info(tag):
    h = find()
    if not h:
        print("未找到窗口")
        return
    cr = wintypes.RECT(); u.GetClientRect(h, ctypes.byref(cr))
    cl = wintypes.POINT(0, 0)
    u.ClientToScreen(h, ctypes.byref(cl))
    wr = wintypes.RECT(); u.GetWindowRect(h, ctypes.byref(wr))
    mon = u.MonitorFromWindow(h, 2)  # NEAREST
    dpi = u.GetDpiForWindow(h) if hasattr(u, "GetDpiForWindow") else 96
    mi = MONITORINFO(); mi.cbSize = ctypes.sizeof(MONITORINFO)
    u.GetMonitorInfoW(mon, ctypes.byref(mi))
    wa = mi.rcWork
    # 外框矩形相对工作区的偏移（最大化时理论应为负的外扩边框）
    print(f"[{tag}] dpi={dpi}")
    print(f"  外框矩形 = ({wr.left},{wr.top})-({wr.right},{wr.bottom}) "
          f"尺寸 {wr.right - wr.left}x{wr.bottom - wr.top}")
    print(f"  客户区(屏幕绝对) = ({cl.x},{cl.y})-({cr.right - cr.left + cl.x},{cr.bottom - cr.top + cl.y}) "
          f"尺寸={cr.right - cr.left}x{cr.bottom - cr.top}")
    print(f"  工作区   = ({wa.left},{wa.top})-({wa.right},{wa.bottom}) "
          f"尺寸={wa.right - wa.left}x{wa.bottom - wa.top}")
    dxl = cl.x - wa.left
    dyt = cl.y - wa.top
    dxr = wa.right - (cr.right - cr.left + cl.x)
    dyb = wa.bottom - (cr.bottom - cr.top + cl.y)
    print(f"  客户区 vs 工作区：左差 {dxl} 上差 {dyt} 右差 {dxr} 下差 {dyb}")
    if dxl == dyt == dxr == dyb == 0:
        print("      → 客户区严丝合缝等于工作区，最大化正确 ✓")
    else:
        print("      → 最大化客户区与工作区不符！")


def main():
    if len(sys.argv) < 2 or sys.argv[1] == "info":
        info("info")
        return 0
    h = find()
    if not h:
        print("未找到窗口")
        return 1
    info("最大化前")
    u.PostMessageW(h, WM_SYSCOMMAND, SC_MAXIMIZE, 0)
    time.sleep(1.0)
    info("最大化后")
    time.sleep(0.3)
    u.PostMessageW(h, WM_SYSCOMMAND, SC_RESTORE, 0)
    time.sleep(0.5)
    info("还原后")
    return 0


if __name__ == "__main__":
    sys.exit(main())
