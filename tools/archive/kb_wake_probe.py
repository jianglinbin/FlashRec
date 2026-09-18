#!/usr/bin/env python3
"""键盘拖动进度是否唤醒底栏的实测探针（d36）。

方法：SendInput 系统级注入 VK_LEFT（带 scancode，区别于 PostMessage 的
无声之谜），抢前台 + 真实投屏起播 + 光标钉在窗口中央（远离底栏热区带），
分别截「基线（面板已淡出）/ 按住左键中 / 释放后」三张，采样两个区域：
  - 播放按钮图标区（面板元素）：亮 = 面板真被唤醒
  - 进度条中部（track_alpha 预览）：亮 = 键盘预览（预期行为）
用法: python tools/archive/kb_wake_probe.py
铁律：不启不杀实例（复用当前实例）；只读采样 + 合成键。
"""
import ctypes
import os
import struct
import subprocess
import sys
import time
import zlib

u32 = ctypes.windll.user32

# DPI 感知必须先行（铁律：app 客户区是物理像素）
try:
    u32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    u32.SetProcessDpiAwarenessContext.restype = ctypes.c_int
    u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)

u32.FindWindowW.restype = ctypes.c_void_p
u32.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]

WM_KEYDOWN, WM_KEYUP, WM_CHAR = 0x0100, 0x0101, 0x0102
VK_LEFT = 0x25
INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_SCANCODE = 0x0008


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", ctypes.c_ushort), ("wScan", ctypes.c_ushort),
                ("dwFlags", ctypes.c_uint), ("time", ctypes.c_uint),
                ("dwExtraInfo", ctypes.c_size_t)]


class _U(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_ubyte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint), ("u", _U)]


def send_vk(vk, up=False):
    sc = u32.MapVirtualKeyW(vk, 0)  # MAPVK_VK_TO_VSC
    ki = KEYBDINPUT(0, sc, (KEYEVENTF_KEYUP if up else 0) | KEYEVENTF_SCANCODE, 0, 0)
    inp = INPUT(INPUT_KEYBOARD, _U(ki))
    u32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


class RECT(ctypes.Structure):
    _fields_ = [("L", ctypes.c_long), ("T", ctypes.c_long), ("R", ctypes.c_long), ("B", ctypes.c_long)]


class POINT(ctypes.Structure):
    _fields_ = [("X", ctypes.c_long), ("Y", ctypes.c_long)]


HWND_TOPMOST, HWND_NOTOPMOST = ctypes.c_void_p(-1), ctypes.c_void_p(-2)
SWP_NOSIZE, SWP_NOMOVE, SWP_NOACTIVATE = 0x0001, 0x0002, 0x0010


def client_size(hwnd):
    r = RECT()
    u32.GetClientRect(hwnd, ctypes.byref(r))
    return r.R - r.L, r.B - r.T


def client_origin(hwnd):
    p = POINT(0, 0)
    u32.ClientToScreen(hwnd, ctypes.byref(p))
    return p.X, p.Y


def grab(x, y, w, h):
    g32, k32 = ctypes.windll.gdi32, ctypes.windll.kernel32
    hdc = u32.GetDC(None)
    mem = g32.CreateCompatibleDC(hdc)
    bmp = g32.CreateCompatibleBitmap(hdc, w, h)
    g32.SelectObject(mem, bmp)
    SRCCOPY = 0x00CC0020
    g32.BitBlt(mem, 0, 0, w, h, hdc, x, y, SRCCOPY)

    class BMIH(ctypes.Structure):
        _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
                    ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                    ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                    ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
                    ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
                    ("biClrImportant", ctypes.c_uint32)]

    class BMI(ctypes.Structure):
        _fields_ = [("bmiHeader", BMIH), ("bmiColors", ctypes.c_uint32 * 3)]

    bi = BMI()
    bi.bmiHeader.biSize = ctypes.sizeof(BMIH)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = h
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    g32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mem)
    u32.ReleaseDC(None, hdc)
    rows = [buf.raw[i * w * 4:(i + 1) * w * 4] for i in range(h)]
    return b"".join(reversed(rows)), w, h


def write_png(path, bgra, w, h):
    px = bytearray(bgra)
    px[0::4], px[2::4] = px[2::4], px[0::4]

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = bytearray()
    for yy in range(h):
        raw.append(0)
        raw += px[yy * w * 4:(yy + 1) * w * 4]
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    out += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(out)


def bright_count(buf, w, x0, y0, x1, y1, thr=140):
    n = 0
    for y in range(int(y0), int(y1)):
        for x in range(int(x0), int(x1)):
            i = (y * w + x) * 4
            if buf[i + 2] > thr and buf[i + 1] > thr and buf[i] > thr:
                n += 1
    return n


def main():
    hwnd = u32.FindWindowW(None, "FlashRec 投屏接收")
    if not hwnd:
        print("FAIL: 未找到 flashrec 窗口（实例未运行？）")
        return 1
    # 起播（复用当前实例；BigBuckBunny 1:22）
    subprocess.run([sys.executable, "tools/selftest_cast.py"],
                   cwd=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   capture_output=True)
    time.sleep(2.5)  # 等 PLAYING
    # 抢前台 + 置顶截图
    u32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE)
    time.sleep(0.3)
    # 光标钉在窗口中央偏上（远离底栏热区带），并触一次醒随后等淡出
    ox, oy = client_origin(hwnd)
    w, h = client_size(hwnd)
    u32.SetCursorPos(ox + w // 2, oy + int(h * 0.35))
    # 晃 20px 触发唤醒，再等 2.8s+ 让面板彻底淡出
    u32.SetCursorPos(ox + w // 2 + 20, oy + int(h * 0.35))
    time.sleep(3.2)

    out_dir = os.path.dirname(os.path.abspath(__file__))
    ox, oy = client_origin(hwnd)
    w, h = client_size(hwnd)
    bgra, w, h = grab(ox, oy, w, h)
    write_png(os.path.join(out_dir, "kb_base.png"), bgra, w, h)
    base_btn = bright_count(bgra, w, int(w * 0.10), h - 120, int(w * 0.16), h - 20)
    base_track = bright_count(bgra, w, int(w * 0.40), h - 70, int(w * 0.60), h - 15)
    print(f"基线      : 按钮区亮={base_btn}  进度条区亮={base_track}")

    # 按住左键 0.9s（键盘拖动进行中）
    send_vk(VK_LEFT)
    time.sleep(0.9)
    bgra, w, h = grab(ox, oy, w, h)
    write_png(os.path.join(out_dir, "kb_hold.png"), bgra, w, h)
    hold_btn = bright_count(bgra, w, int(w * 0.10), h - 120, int(w * 0.16), h - 20)
    hold_track = bright_count(bgra, w, int(w * 0.40), h - 70, int(w * 0.60), h - 15)
    print(f"按住左键中: 按钮区亮={hold_btn}  进度条区亮={hold_track}")

    send_vk(VK_LEFT, up=True)
    time.sleep(0.25)
    bgra, w, h = grab(ox, oy, w, h)
    write_png(os.path.join(out_dir, "kb_rel.png"), bgra, w, h)
    rel_btn = bright_count(bgra, w, int(w * 0.10), h - 120, int(w * 0.16), h - 20)
    rel_track = bright_count(bgra, w, int(w * 0.40), h - 70, int(w * 0.60), h - 15)
    print(f"释放后    : 按钮区亮={rel_btn}  进度条区亮={rel_track}")

    u32.SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE)

    print("\n结论：")
    if hold_btn > base_btn + 50:
        print("  [FAIL] 按住左键时播放按钮区变亮 → 面板真被唤醒（main.cpp 键盘路径仍有 touch）")
    else:
        print("  [OK] 面板未唤醒（按钮区无变化）")
    print(f"  进度条区差值 {hold_track - base_track}（>0 = track_alpha 预览生效，预期）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
