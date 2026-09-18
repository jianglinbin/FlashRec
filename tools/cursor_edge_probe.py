#!/usr/bin/env python3
"""验证 d39 统一拖拽的边缘光标暗示：把**真实**光标移到窗口边缘带，采样系统
当前光标句柄，与标准光标句柄比对，报告"左缘是否显示 ↔、上缘是否显示 ↕"。

关键点：
- WM_SETCURSOR 只在**真实**鼠标输入（SetCursorPos 产生的系统级 move）派发时
  才被系统发送，PostMessage 合成的 WM_MOUSEMOVE 不触发 —— 所以必须真实移动。
- 连续 SetCursorPos 到同一点不会再产生 move；先移到"预备点"再移到目标点。
- DPI 铁律：任何窗口 API 之前先 SetProcessDpiAwareness(2)（app 客户区是物理像素）。

用法:
  python tools/cursor_edge_probe.py            # 全部位置采样一遍
  python tools/cursor_edge_probe.py left       # 只测左缘
"""
import ctypes
import sys
import time
from ctypes import wintypes

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)  # PER_MONITOR_DPI_AWARE
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

user32 = ctypes.windll.user32
TITLE = "FlashRec 投屏接收"

# 标准光标资源 ID → 名称（比对用）。注意：GLFW 用 LoadImageW(LR_SHARED) 创建标准
# 光标，句柄与 LoadCursorW 可能不同 —— 探针必须用 LoadImageW 同参数取句柄才可比。
IDC = {
    32512: "箭头(ARROW)",
    32645: "上下双箭头(SIZENS)",
    32646: "左右双箭头(SIZEWE)",
    32642: "左斜双箭头(SIZENWSE)",
    32643: "右斜双箭头(SIZENESW)",
}

IMAGE_CURSOR = 2
LR_DEFAULTSIZE = 0x40
LR_SHARED = 0x8000


def load_cursor_handle(cid):
    # 与 GLFW 完全同参：LoadImageW(NULL, MAKEINTRESOURCEW(id), IMAGE_CURSOR, 0,0, LR_DEFAULTSIZE|LR_SHARED)
    return ctypes.windll.user32.LoadImageW(None, ctypes.c_wchar_p(cid),
                                           IMAGE_CURSOR, 0, 0,
                                           LR_DEFAULTSIZE | LR_SHARED)


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class CURSORINFO(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_uint), ("flags", ctypes.c_uint),
                ("hCursor", ctypes.wintypes.HANDLE),
                ("ptScreenPos", POINT)]


def find_window():
    hwnd = user32.FindWindowW(None, TITLE)
    if hwnd:
        return hwnd
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


def move_to_screen(sx, sy, from_dx=-24, from_dy=-18):
    """真实移动光标：先经过预备点再落目标点（同点重复不产生 move 事件）"""
    user32.SetCursorPos(max(0, sx + from_dx), max(0, sy + from_dy))
    time.sleep(0.08)
    user32.SetCursorPos(sx, sy)
    time.sleep(0.15)


def sample_cursor():
    ci = CURSORINFO()
    ci.cbSize = ctypes.sizeof(CURSORINFO)
    user32.GetCursorInfo(ctypes.byref(ci))
    return ci.hCursor, ci.ptScreenPos.x, ci.ptScreenPos.y


def main():
    hwnd = find_window()
    if not hwnd:
        print("未找到 FlashRec 窗口（app 未运行？）")
        return 1

    cr = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(cr))
    w, h = cr.right - cr.left, cr.bottom - cr.top
    zoomed = user32.IsZoomed(hwnd)
    print(f"hwnd=0x{hwnd:X} client={w}x{h} maximized={bool(zoomed)}")
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.3)

    # 距边 3~4px（在 6px 直边带内）；角落 (3,3)（在 18px 贴角带内）
    targets = {
        "center":    (w // 2, h // 2, "箭头"),
        "left":      (3, h // 2, "左右双箭头"),
        "right":     (w - 4, h // 2, "左右双箭头"),
        "top":       (w // 2, 3, "上下双箭头"),
        "bottom":    (w // 2, h - 4, "上下双箭头"),
        "corner-NW": (9, 9, "左斜双箭头"),
        "corner-SE": (w - 10, h - 10, "左斜双箭头"),
    }
    only = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None
    if only:
        targets = {k: v for k, v in targets.items() if k == only}

    # 标准光标句柄 → 名称反查表（LoadImageW 同源）
    handle2name = {}
    for cid, cname in IDC.items():
        h = load_cursor_handle(cid)
        handle2name[h] = cname
    handle2name[0] = "(空=隐藏)"

    print(f"{'位置':<10} {'期望':<30} 实际")
    fail = 0
    for name, (cx, cy, expect) in targets.items():
        p = POINT(cx, cy)
        user32.ClientToScreen(hwnd, ctypes.byref(p))
        move_to_screen(p.x, p.y)
        hcur, gx, gy = sample_cursor()
        actual = handle2name.get(hcur, f"未知(0x{hcur or 0:X})")
        ok = (expect == actual)
        if not ok:
            fail += 1
        print(f"{name:<10} {expect:<30} {actual}  {'✓' if ok else '✗'}")
    print("结论:", "存在未生效位置 ✗" if fail else "全部符合预期 ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
