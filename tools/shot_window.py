#!/usr/bin/env python3
"""截取 flashrec 主窗口的可视区域，用于人工核对渲染结果（镜像 / 底栏 / 圆角）。

只读操作：不点击、不注入、不改变窗口状态。用 PrintWindow(PW_RENDERFULLCONTENT)
抓取窗口自身内容，避免被其它窗口遮挡导致截图里混进桌面内容。

用法:
  python tools/shot_window.py [输出路径]     # 默认 tools/archive/shot.png
"""
import ctypes
import os
import sys
from ctypes import wintypes

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "archive", "shot.png")


def _become_dpi_aware():
    """必须在任何窗口 API 之前调用！

    FlashRec 是 per-monitor DPI aware 进程（GLFW 在 _glfwInitWin32 里设的），
    它的客户区尺寸是**物理像素**（如 1500x875 @125%）。而本脚本默认是 DPI UNAWARE：
    Windows 会对它虚拟化 GetWindowRect/GetClientRect，返回**缩小后的逻辑尺寸**
    （125% 下 1500→1200），于是 PrintWindow 只抓到了窗口左上角一块 ——
    底栏、右边缘全被切掉，看起来就像"底栏没画出来"（实际是截图工具的问题）。
    设为 PER_MONITOR_DPI_AWARE 后读数与 app 内部一致。
    """
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)  # PER_MONITOR_DPI_AWARE
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()   # 旧系统回退
        except Exception:
            pass


_become_dpi_aware()

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

PW_RENDERFULLCONTENT = 0x00000002


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", ctypes.c_long),
        ("biHeight", ctypes.c_long),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", ctypes.c_long),
        ("biYPelsPerMeter", ctypes.c_long),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


def find_hwnd():
    """枚举顶层窗口，按标题/类名找 flashrec。"""
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _lp):
        if not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if n <= 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        title = buf.value
        if "FlashRec" in title or "flashrec" in title.lower():
            found.append((hwnd, title))
        return True

    user32.EnumWindows(cb, 0)
    return found


def capture(hwnd, path):
    rc = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rc))
    # 客户区 + 窗口边框（无边框窗口两者一致；留 1px 兜底避免 0 尺寸）
    w = max(1, rc.right - rc.left)
    h = max(1, rc.bottom - rc.top)

    hdc = user32.GetDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)

    ok = user32.PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)

    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h  # 负数 = 自上而下
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bi.bmiHeader.biCompression = 0  # BI_RGB

    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)

    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)
    return ok, w, h, buf.raw


def main():
    wins = find_hwnd()
    if not wins:
        print("未找到 flashrec 窗口（应用未运行？）")
        return 1
    hwnd, title = wins[0]
    ok, w, h, raw = capture(hwnd, OUT)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)

    # 用 PIL 落盘（有则用；没有则写裸 BMP 头，保证不依赖额外包）
    try:
        from PIL import Image
        img = Image.frombytes("RGBA", (w, h), raw, "raw", "BGRA")
        img.save(OUT)
        print(f"已保存 {OUT}  ({w}x{h})  PrintWindow={bool(ok)}  title={title!r}")
        return 0
    except ImportError:
        pass

    # 手写 BMP（24bpp，去 BGRA→BGR）
    row = w * 3
    pad = (4 - row % 4) % 4
    size = 54 + (row + pad) * h
    with open(OUT, "wb") as f:
        f.write(b"BM" + size.to_bytes(4, "little") + b"\0\0\0\0" + (54).to_bytes(4, "little"))
        f.write((40).to_bytes(4, "little") + w.to_bytes(4, "little", signed=True) +
                (-h).to_bytes(4, "little", signed=True) + (1).to_bytes(2, "little") +
                (24).to_bytes(2, "little") + b"\0" * 24)
        for y in range(h):
            base = y * w * 4
            line = bytearray()
            for x in range(w):
                o = base + x * 4
                line += raw[o:o + 3]  # BGR 顺序已符合
            f.write(bytes(line) + b"\0" * pad)
    print(f"已保存 {OUT}  ({w}x{h})  PrintWindow={bool(ok)}  title={title!r}  [无 PIL，写 BMP]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
