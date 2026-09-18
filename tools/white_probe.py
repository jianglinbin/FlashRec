#!/usr/bin/env python3
"""白屏唯一判据：GDI 像素回读 FlashRec 客户区，统计近白像素占比。

用法: python tools/white_probe.py
"""
import ctypes
from ctypes import wintypes
import sys

u, g, k = ctypes.windll.user32, ctypes.windll.gdi32, ctypes.windll.kernel32


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG),
                ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


def find_hwnd():
    """按进程名 flashrec.exe 枚举可见主窗口（播放中标题会变成视频名）。"""
    k32 = ctypes.windll.kernel32
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

    def is_flashrec(pid):
        h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not h:
            return False
        try:
            name = ctypes.create_unicode_buffer(520)
            sz = wintypes.DWORD(520)
            if k32.QueryFullProcessImageNameW(h, 0, name, ctypes.byref(sz)):
                return "flashrec" in name.value.lower()
        finally:
            k32.CloseHandle(h)
        return False

    hit = []
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hw, _l):
        pid = wintypes.DWORD(0)
        u.GetWindowThreadProcessId(hw, ctypes.byref(pid))
        if pid.value and is_flashrec(pid.value) and u.IsWindowVisible(hw):
            hit.append(hw)
        return True
    u.EnumWindows(cb, 0)
    return hit[0] if hit else 0


def main():
    hwnd = find_hwnd()
    if not hwnd:
        print("窗口未找到")
        return 1
    rc = wintypes.RECT()
    u.GetClientRect(hwnd, ctypes.byref(rc))
    w, h = rc.right, rc.bottom
    hdc = u.GetDC(hwnd)
    mem = g.CreateCompatibleDC(hdc)
    bmp = g.CreateCompatibleBitmap(hdc, w, h)
    g.SelectObject(mem, bmp)
    # BitBlt 屏幕内容（PrintWindow 对 GL 层不可靠，用屏幕 DC）
    u.SetProcessDPIAware()
    pt = wintypes.POINT(0, 0)
    u.ClientToScreen(hwnd, ctypes.byref(pt))
    sdc = u.GetDC(0)
    g.BitBlt(mem, 0, 0, w, h, sdc, pt.x, pt.y, 0x00CC0020)  # SRCCOPY
    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth, bi.biHeight, bi.biPlanes, bi.biBitCount = w, -h, 1, 32
    bi.biCompression = 0
    buf = ctypes.create_string_buffer(w * h * 4)
    g.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
    data = bytearray(buf.raw)  # Python 3：string buffer 索引返回 bytes，转 bytearray 拿 int
    # 判据（d151 起三态）：白屏 / 黑屏 / 有画面
    #   · 白屏 = 近白占比 > 30%（GL 纹理建不出来时的典型形态）
    #   · 黑屏 = 近黑占比 > 85% 且颜色种类少（降级后未重载、无帧的典型形态）
    #   · 有画面 = 颜色多样性足够（唯一颜色数 > 200）
    total = w * h
    white = 0
    near_white = 0
    near_black = 0
    colors = set()
    step = 4
    for y in range(0, h, step):
        row = y * w * 4
        for x in range(0, w, step):
            i = row + x * 4
            b, gg, r = data[i], data[i + 1], data[i + 2]
            if r >= 245 and gg >= 245 and b >= 245:
                white += 1
            if r >= 235 and gg >= 235 and b >= 235:
                near_white += 1
            if r <= 20 and gg <= 20 and b <= 20:
                near_black += 1
            colors.add(((r >> 3) << 10) | ((gg >> 3) << 5) | (b >> 3))
    sampled = len(range(0, w, step)) * len(range(0, h, step))
    pw = 100 * near_white / sampled
    pb = 100 * near_black / sampled
    print(f"客户区 {w}x{h}  采样 {sampled} 点")
    print(f"  近白(>=235): {pw:.1f}%   近黑(<=20): {pb:.1f}%   唯一颜色数: {len(colors)}")
    if len(colors) > 200 and pw < 30:
        verdict = "★有画面（内容已渲染）"
    elif pw >= 30:
        verdict = "★★白屏（视频未渲染）"
    else:
        verdict = "★★黑屏/无画面（颜色单调）"
    print("判定:", verdict)
    g.DeleteObject(bmp)
    g.DeleteDC(mem)
    u.ReleaseDC(hwnd, hdc)
    u.ReleaseDC(0, sdc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
