#!/usr/bin/env python3
"""用 SetWindowPos 改变 FlashRec 窗口尺寸，供尺寸链实测（win/fbo_content/fbo_alloc）。

只改窗口大小，不动内容。用法:
  python tools/resize_window.py 1400 800
  python tools/resize_window.py maximize
  python tools/resize_window.py fullscreen-hint
"""
import ctypes
import sys
from ctypes import wintypes

u = ctypes.windll.user32
TITLE = "FlashRec 投屏接收"
SWP_NOZORDER, SWP_NOACTIVATE = 0x0004, 0x0010


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


def main():
    h = find()
    if not h:
        print("未找到窗口")
        return 1
    if len(sys.argv) < 2:
        print("用法: resize_window.py <宽> <高> | maximize")
        return 2
    if sys.argv[1] == "maximize":
        u.ShowWindow(h, 3)  # SW_MAXIMIZE
        print("已最大化")
        return 0
    w, hh = int(sys.argv[1]), int(sys.argv[2])
    r = wintypes.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    u.SetWindowPos(h, 0, r.left, r.top, w, hh, SWP_NOZORDER | SWP_NOACTIVATE)
    print(f"已设置为 {w}x{hh}（窗口外框尺寸）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
