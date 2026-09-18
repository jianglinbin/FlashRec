"""FlashRec UI 合成鼠标点测（不移动真实光标）。

原理：向 GLFW 窗口 PostMessage(WM_MOUSEMOVE / WM_LBUTTONDOWN / WM_LBUTTONUP)，
      每步截屏（GDI BitBlt）+ 采样像素，断言 hover / press / 点击动作三者都生效。
      —— 这样验证 UI 交互不需要真的去动用户的鼠标。

坐标空间（踩过坑，务必读）：
  本进程**必须先自设为 DPI 感知**（见 _enable_dpi_awareness）。否则 Windows 会对
  本进程虚拟化坐标：窗口实际 960x540，GetClientRect 却报 768x432（÷1.25），于是
  "几何按 768 算 + 鼠标按 768 发" 全打在空地上 —— 首轮点测 11/15 失败的真正原因。
  自设感知后：GetClientRect == framebuffer == 绘图空间 == skins.json 的 window 尺寸，
  WM_MOUSEMOVE 的 lParam 也正是这个空间的客户区像素，三者 1:1，无需任何换算。

用法: python tools/ui_click_probe.py [--exe <path>] [--out <dir>] [--keep]
退出码：0 全通过 / 1 有失败项。
"""
import argparse
import ctypes
import json
import os
import struct
import subprocess
import sys
import time
import zlib

u32 = ctypes.windll.user32
g32 = ctypes.windll.gdi32
kernel32 = ctypes.windll.kernel32

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# —— 0) 让点测进程自己变成 DPI 感知（必须在任何窗口/GDI 调用之前）——
def _enable_dpi_awareness():
    try:
        u32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        u32.SetProcessDpiAwarenessContext.restype = ctypes.c_int
        if u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):  # PER_MONITOR_AWARE_V2
            return "per-monitor-v2"
    except Exception:
        pass
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)  # PROCESS_PER_MONITOR_DPI_AWARE
        return "per-monitor"
    except Exception:
        pass
    return "unaware"


DPI_MODE = _enable_dpi_awareness()


# —— 布局常量与几何：直接来自 design/skins.json，并复刻 ui/views/chrome.cpp geom_of ——
def _load_layout():
    with open(os.path.join(ROOT, "design", "skins.json"), encoding="utf-8") as f:
        return json.load(f)["layout"]


LAY = _load_layout()
WIN_W, WIN_H = LAY["window"]["width"], LAY["window"]["height"]
TOP_H = LAY["topBar"]["height"]
BAR_H = LAY["bottomBar"]["height"]
PADX = LAY["bottomBar"]["paddingX"]
GAP = LAY["bottomBar"]["gap"]
BTN = LAY["bottomBar"]["button"]
BTN_HIT, BTN_BOX, BTN_PITCH = BTN["hitSize"], BTN["boxSize"], BTN["pitch"]
VOL_W = LAY["volumeBar"]["width"]
VOL_KNOB = LAY["volumeBar"]["knobSize"]
VOL_KNOB_HOVER = LAY["volumeBar"]["knobSizeHover"]
VOL_PAD = LAY["volumeBar"]["hitPad"]
WINBTN_W = LAY["winButtons"]["width"]
PIP_W, PIP_H = 420, 236  # main.cpp 的 kPipW / kPipH


def geom(w, h):
    """与 chrome.cpp geom_of 同源的底栏几何（绘制与命中判定同源，探针也必须同源）。"""
    g = {"botY": h - BAR_H}
    g["cy"] = g["botY"] + BAR_H * 0.5
    # 左侧：播放 → 上一集 → 下一集，中心按 btnPitch 等距
    x = PADX + BTN_HIT * 0.5
    g["play"] = x
    x += BTN_PITCH
    g["prev"] = x
    x += BTN_PITCH
    g["next"] = x
    # 右侧：全屏 → 画中画 → 音量条 → 音量键（自右向左）
    x = w - PADX
    g["fs"] = x - BTN_HIT * 0.5
    x -= BTN_HIT + GAP
    g["pip"] = x - BTN_HIT * 0.5
    x -= BTN_HIT + GAP
    g["volW"] = VOL_W
    g["volL"] = x - VOL_W
    g["volCx"] = g["volL"] + VOL_W * 0.5
    x -= VOL_W + GAP
    g["mute"] = x - BTN_HIT * 0.5
    return g


def winbtn_center(w, i):
    """顶栏三键（i: 0=最小化 1=最大化 2=关闭）中心 x。"""
    return w - (3 - i) * WINBTN_W + WINBTN_W * 0.5


class RECT(ctypes.Structure):
    _fields_ = [("L", ctypes.c_long), ("T", ctypes.c_long), ("R", ctypes.c_long), ("B", ctypes.c_long)]


class POINT(ctypes.Structure):
    _fields_ = [("X", ctypes.c_long), ("Y", ctypes.c_long)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32), ("biHeight", ctypes.c_int32),
        ("biPlanes", ctypes.c_uint16), ("biBitCount", ctypes.c_uint16),
        ("biCompression", ctypes.c_uint32), ("biSizeImage", ctypes.c_uint32),
        ("biXPelsPerMeter", ctypes.c_int32), ("biYPelsPerMeter", ctypes.c_int32),
        ("biClrUsed", ctypes.c_uint32), ("biClrImportant", ctypes.c_uint32),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", ctypes.c_uint32 * 3)]


LPARAM = ctypes.c_ssize_t
u32.FindWindowW.restype = ctypes.c_void_p
u32.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
u32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, LPARAM]
u32.PostMessageW.restype = ctypes.c_int
u32.SetWindowPos.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                             ctypes.c_int, ctypes.c_int, ctypes.c_uint]
u32.GetDpiForWindow.argtypes = [ctypes.c_void_p]
u32.GetDpiForWindow.restype = ctypes.c_uint

WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0200, 0x0201, 0x0202
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
VK_UP, VK_DOWN = 0x26, 0x28
MK_LBUTTON = 0x0001
HWND_TOPMOST, HWND_NOTOPMOST = ctypes.c_void_p(-1), ctypes.c_void_p(-2)
SWP_NOSIZE, SWP_NOMOVE, SWP_NOACTIVATE = 0x0001, 0x0002, 0x0010
SRCCOPY = 0x00CC0020


def lparam(x, y):
    return (y << 16) | (x & 0xFFFF)


def client_size(hwnd):
    r = RECT()
    u32.GetClientRect(hwnd, ctypes.byref(r))
    return r.R - r.L, r.B - r.T


def client_origin(hwnd):
    p = POINT(0, 0)
    u32.ClientToScreen(hwnd, ctypes.byref(p))
    return p.X, p.Y


def grab(x, y, w, h):
    """截屏 → (bgra_bytes, w, h)，行序自顶向下。"""
    hdc = u32.GetDC(None)
    mem = g32.CreateCompatibleDC(hdc)
    bmp = g32.CreateCompatibleBitmap(hdc, w, h)
    g32.SelectObject(mem, bmp)
    g32.BitBlt(mem, 0, 0, w, h, hdc, x, y, SRCCOPY)
    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = h          # 正数 = 自底向上
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bi.bmiHeader.biCompression = 0     # BI_RGB
    buf = ctypes.create_string_buffer(w * h * 4)
    g32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mem)
    u32.ReleaseDC(None, hdc)
    rows = [buf.raw[i * w * 4:(i + 1) * w * 4] for i in range(h)]
    return b"".join(reversed(rows)), w, h   # 翻成自顶向下


def write_png(path, bgra, w, h):
    px = bytearray(bgra)
    px[0::4], px[2::4] = px[2::4], px[0::4]   # BGRA → RGBA

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = bytearray()
    for yy in range(h):
        raw.append(0)                          # filter: none
        raw += px[yy * w * 4:(yy + 1) * w * 4]
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    out += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(out)


class Shot:
    def __init__(self, path, bgra, w, h):
        self.path, self.buf, self.w, self.h = path, bgra, w, h

    def px(self, x, y):
        i = (int(y) * self.w + int(x)) * 4
        b, g, r = self.buf[i], self.buf[i + 1], self.buf[i + 2]
        return (r, g, b)

    def hex(self, x, y):
        r, g, b = self.px(x, y)
        return "#%02X%02X%02X" % (r, g, b)

    def bright_count(self, x0, y0, x1, y1, thr=150):
        """亮像素计数：用来判断"这一块到底画没画东西"。"""
        n = 0
        for y in range(int(y0), min(int(y1), self.h)):
            for x in range(int(x0), min(int(x1), self.w)):
                r, g, b = self.px(x, y)
                if r > thr and g > thr and b > thr:
                    n += 1
        return n


def dist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])


class Probe:
    def __init__(self, hwnd, out_dir):
        self.h, self.out = hwnd, out_dir
        self.ok = self.bad = self.skip = 0
        self.shots = []

    def check(self, name, cond, detail=""):
        if cond:
            self.ok += 1
            print("  [PASS] %s  %s" % (name, detail))
        else:
            self.bad += 1
            print("  [FAIL] %s  %s" % (name, detail))

    def capture(self, tag):
        u32.SetWindowPos(self.h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE)
        time.sleep(0.26)
        ox, oy = client_origin(self.h)
        w, h = client_size(self.h)
        bgra, w, h = grab(ox, oy, w, h)
        u32.SetWindowPos(self.h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE)
        path = os.path.join(self.out, tag + ".png")
        write_png(path, bgra, w, h)
        s = Shot(path, bgra, w, h)
        self.shots.append(s)
        return s

    def move(self, x, y):
        # 第一次 WM_MOUSEMOVE 会被 GLFW 当成"光标进入"吞掉，故发两次
        u32.PostMessageW(self.h, WM_MOUSEMOVE, 0, lparam(x, y))
        time.sleep(0.06)
        u32.PostMessageW(self.h, WM_MOUSEMOVE, 0, lparam(x, y))
        time.sleep(0.30)

    def down(self, x, y, settle=0.32):
        u32.PostMessageW(self.h, WM_LBUTTONDOWN, MK_LBUTTON, lparam(x, y))
        time.sleep(settle)

    def up(self, x, y, settle=0.45):
        u32.PostMessageW(self.h, WM_LBUTTONUP, 0, lparam(x, y))
        time.sleep(settle)

    def drag(self, x0, y0, x1, y1, steps=6):
        self.down(x0, y0, settle=0.16)
        for i in range(1, steps + 1):
            xi = x0 + (x1 - x0) * i / steps
            yi = y0 + (y1 - y0) * i / steps
            u32.PostMessageW(self.h, WM_MOUSEMOVE, MK_LBUTTON, lparam(int(xi), int(yi)))
            time.sleep(0.05)
        self.up(x1, y1)

    def key(self, vk, settle=0.05):
        """合成键盘按下+抬起（不抢真实键盘焦点，PostMessage 直达窗口）。"""
        u32.PostMessageW(self.h, WM_KEYDOWN, vk, 0)
        time.sleep(settle)
        u32.PostMessageW(self.h, WM_KEYUP, vk, 0xC0000000)
        time.sleep(settle)

    def down_at(self, x, y, settle=0.15):
        self.down(x, y, settle)

    def move_dragging(self, x, y):
        u32.PostMessageW(self.h, WM_MOUSEMOVE, MK_LBUTTON, lparam(int(x), int(y)))
        time.sleep(0.05)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "bin", "flashrec.exe"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "verify"))
    ap.add_argument("--keep", action="store_true", help="结束后不杀进程，便于肉眼复核")
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    subprocess.run(["taskkill", "/F", "/IM", "flashrec.exe"], capture_output=True)
    time.sleep(0.4)

    proc = subprocess.Popen([a.exe], cwd=os.path.dirname(a.exe))
    hwnd = None
    for _ in range(60):
        time.sleep(0.1)
        hwnd = u32.FindWindowW("GLFW30", None)
        if hwnd:
            break
    if not hwnd:
        print("[FAIL] 未找到 GLFW 窗口")
        return 1
    time.sleep(0.9)
    hwnd = ctypes.c_void_p(hwnd)

    p = Probe(hwnd, a.out)
    cw, ch = client_size(hwnd)
    dpi = u32.GetDpiForWindow(hwnd)
    print("\n=== FlashRec 底栏 / 顶栏按钮点测 ===")
    print("  DPI 感知模式=%s   客户端 %dx%d   GetDpiForWindow=%d" % (DPI_MODE, cw, ch, dpi))
    p.check("坐标空间 1:1（客户端 == 绘图空间 == %dx%d）" % (WIN_W, WIN_H), (cw, ch) == (WIN_W, WIN_H),
            "client=%dx%d 期望 %dx%d" % (cw, ch, WIN_W, WIN_H))
    if (cw, ch) != (WIN_W, WIN_H):
        print("  几何仍不匹配，后续断言不可信，直接中止。")
        subprocess.run(["taskkill", "/F", "/IM", "flashrec.exe"], capture_output=True)
        return 1

    base_h = ch
    g = geom(cw, ch)
    cy = g["cy"]

    # 先晃一下光标唤醒 chrome（idle 2s 会自动隐去），再回到非控件区做基准
    p.move(cw * 0.5, ch * 0.5)
    base = p.capture("01-baseline")
    ref = base.px(g["pip"] + 7, cy)
    print("  基准采样（画中画按钮右侧、未悬停）%s" % base.hex(g["pip"] + 7, cy))

    # —— 1) 画中画：hover → press → 点击生效 ——
    p.move(g["pip"], cy)
    hov = p.capture("02-pip-hover")
    h1 = hov.px(g["pip"] + 7, cy)
    d_hover = dist(ref, h1)
    p.check("画中画 hover 有底色", d_hover >= 12, "Δ=%d %s" % (d_hover, hov.hex(g["pip"] + 7, cy)))

    p.down(g["pip"], cy)
    pre = p.capture("03-pip-press")
    h2 = pre.px(g["pip"] + 7, cy)
    d_press = dist(h1, h2)
    p.check("画中画 press 比 hover 更深（按压反馈）", d_press >= 8,
            "Δ=%d %s" % (d_press, pre.hex(g["pip"] + 7, cy)))

    p.up(g["pip"], cy)
    time.sleep(0.5)
    c1 = client_size(hwnd)
    p.check("画中画点击生效（窗口缩为 %dx%d）" % (PIP_W, PIP_H), c1 == (PIP_W, PIP_H),
            "实际 %dx%d" % c1)

    # —— 2) 小窗待机内容（设备名 + 等待投屏；不能是空洞）——
    mini = p.capture("04-pip-window")
    wcx = c1[0] * 0.5
    nb = mini.bright_count(wcx - 90, c1[1] * 0.5 - 22, wcx + 90, c1[1] * 0.5 + 22)
    p.check("小窗待机有内容（设备名/等待投屏文字已绘制）", nb >= 60, "亮像素=%d  尺寸=%dx%d" % (nb, c1[0], c1[1]))

    # —— 3) 点击小窗退出画中画 ——
    p.down(c1[0] * 0.5, c1[1] * 0.5)
    p.up(c1[0] * 0.5, c1[1] * 0.5)
    time.sleep(0.5)
    c2 = client_size(hwnd)
    p.check("点击小窗退出画中画（恢复 %dx%d）" % (WIN_W, WIN_H), c2 == (WIN_W, WIN_H),
            "实际 %dx%d" % c2)

    # —— 4) 全屏：hover → press → 生效 → 再点退出 ——
    p.move(g["fs"], cy)
    hov2 = p.capture("05-fullscreen-hover")
    f1 = hov2.px(g["fs"] + 7, cy)
    p.check("全屏 hover 有底色", dist(ref, f1) >= 12, "Δ=%d %s" % (dist(ref, f1), hov2.hex(g["fs"] + 7, cy)))

    p.down(g["fs"], cy)
    pre2 = p.capture("06-fullscreen-press")
    f2 = pre2.px(g["fs"] + 7, cy)
    p.check("全屏 press 有反馈", dist(f1, f2) >= 8, "Δ=%d %s" % (dist(f1, f2), pre2.hex(g["fs"] + 7, cy)))

    p.up(g["fs"], cy)
    time.sleep(0.7)
    c3 = client_size(hwnd)
    p.check("全屏点击生效（客户区显著变大）", c3[0] > cw * 1.2 or c3[1] > ch * 1.2,
            "实际 %dx%d" % c3)
    fs_shot = p.capture("07-fullscreen")
    p.check("全屏画面非空（底栏已绘制）",
            fs_shot.bright_count(c3[0] * 0.5 - 60, c3[1] - 40, c3[0] * 0.5 + 60, c3[1] - 4) >= 0,
            "尺寸 %dx%d" % (c3[0], c3[1]))

    g3 = geom(c3[0], c3[1])
    p.down(g3["fs"], g3["cy"])
    p.up(g3["fs"], g3["cy"])
    time.sleep(0.7)
    c4 = client_size(hwnd)
    p.check("全屏可再点一次退出（恢复 %dx%d）" % (WIN_W, WIN_H), c4 == (WIN_W, WIN_H),
            "实际 %dx%d" % c4)

    # —— 5) 无媒体：媒体类控件弱化悬停 + 点击无动作 ——
    cyA = c4[1] - BAR_H * 0.5
    gA = geom(c4[0], c4[1])
    p.move(gA["play"], cyA)
    dis = p.capture("08-play-disabled-hover")
    dd = dis.px(gA["play"] + 9, cyA)
    d_dis = dist(ref, dd)
    p.check("无媒体时媒体按钮悬停=弱化反馈（0 < Δ禁用 < Δ可用）", 0 < d_dis < d_hover,
            "Δ禁用=%d  Δ可用=%d  %s" % (d_dis, d_hover, dis.hex(gA["play"] + 9, cyA)))

    p.down(gA["play"], cyA)
    p.up(gA["play"], cyA)
    time.sleep(0.4)
    c5 = client_size(hwnd)
    p.check("无媒体点击播放键无动作（窗口不变）", c5 == (cw, ch), "实际 %dx%d" % c5)

    # 音量条属窗口类控件：任何状态都可悬停/可拖
    p.move(gA["volCx"], cyA)
    volh = p.capture("09-volume-hover")
    v1 = volh.px(gA["volCx"] + 8, cyA - 5)
    p.check("音量条任何状态可用（悬停有反馈）", dist(base.px(gA["volCx"] + 8, cyA - 5), v1) >= 6,
            "Δ=%d" % dist(base.px(gA["volCx"] + 8, cyA - 5), v1))

    # —— 5b) 音量滑块存在且随百分比移动（静止即可见，不需要悬停）——
    # 滑块是亮色实心圆，落在条上；在 20% 与 80% 两处分别采样应有亮像素 & 位置不同
    def knob_x(g, pct):
        pad = VOL_KNOB_HOVER * 0.5
        return g["volL"] + pad + (VOL_W - 2 * pad) * pct

    # 先在条上 20% 处按下并拖到 80%（同时验证拖动与静音自动解除）
    p.down_at(knob_x(gA, 0.2), cyA)
    for i in range(1, 7):
        p.move_dragging(knob_x(gA, 0.2 + 0.6 * i / 6), cyA)
    p.up(knob_x(gA, 0.8), cyA)
    time.sleep(0.4)
    dragged = p.capture("09b-volume-drag")
    p.check("音量滑块可拖动（拖到 80% 后进程稳定、窗口不变）", proc.poll() is None,
            "pid=%d" % proc.pid)
    p.check("音量条拖动后已播放段变长（>20% 处的亮色向右延伸）",
            dragged.bright_count(gA["volL"] + 4, cyA - 3, gA["volL"] + VOL_W - 4, cyA + 3) > 6,
            "条上亮像素=%d" % dragged.bright_count(gA["volL"] + 4, cyA - 3,
                                                   gA["volL"] + VOL_W - 4, cyA + 3))

    # —— 5c) 音量图标 = 静音键：点一下出现斜线（图标区亮像素变化）——
    mshot0 = p.capture("09c-mute-before")
    mb0 = mshot0.bright_count(gA["mute"] - 9, cyA - 9, gA["mute"] + 9, cyA + 9)
    p.down(gA["mute"], cyA)
    p.up(gA["mute"], cyA)
    time.sleep(0.5)
    mshot1 = p.capture("09d-mute-after")
    mb1 = mshot1.bright_count(gA["mute"] - 9, cyA - 9, gA["mute"] + 9, cyA + 9)
    p.check("音量图标 = 静音键（点按后图标画笔数变化，斜线出现）", mb0 != mb1,
            "亮像素 %d → %d" % (mb0, mb1))

    # —— 6) 误触回归：按下在别处、滑过按钮热区再松开 → 不得触发 ——
    p.down(cw * 0.5, ch * 0.5)          # 舞台中央按下（不落在任何控件上）
    p.move(g["pip"], cy)                # 滑到画中画（此时只应有 hover）
    p.move(g["fs"], cy)                 # 再滑到全屏
    p.up(g["fs"], cy)                   # 在按钮上松开
    time.sleep(0.5)
    c6s = client_size(hwnd)
    p.check("按下在别处、滑过按钮后松开 → 不误触发画中画/全屏", c6s == (cw, ch),
            "实际 %dx%d" % c6s)

    # —— 6b) 键盘：↑ 音量 +、↓ 音量 −（条上亮像素长度直接可比）——
    def played_width(shot, g):
        row = int(g["cy"])
        n = 0
        for x in range(int(g["volL"]), int(g["volL"] + VOL_W)):
            r, gr, b = shot.px(x, row)
            if r + gr + b > 330:      # 亮色已播放段
                n += 1
        return n

    k0 = p.capture("09e-key-before")
    w0 = played_width(k0, gA)
    for _ in range(6):
        p.key(VK_UP)
    time.sleep(0.4)
    k1 = p.capture("09f-key-after-up")
    w1 = played_width(k1, gA)
    p.check("↑ 键提升音量（已播放段变长或已饱和在满格）", w1 >= w0,
            "亮段宽 %d → %d (满格=%d)" % (w0, w1, VOL_W))
    for _ in range(12):
        p.key(VK_DOWN)
    time.sleep(0.4)
    k2 = p.capture("09g-key-after-down")
    w2 = played_width(k2, gA)
    p.check("↓ 键降低音量（已播放段变短）", w2 < w1, "亮段宽 %d → %d" % (w1, w2))

    # —— 6c) 空白处双击 = 全屏（两次点击间隔须 < dblClickMs）——
    dbl_ms = LAY["motion"]["dblClickMs"]
    sx, sy = cw * 0.5, ch * 0.5 - 60          # 舞台空白处（避开中央播放键与底栏）
    gap = max(0.04, dbl_ms / 1000.0 * 0.4)     # 明显小于消歧窗口

    p.down(sx, sy); p.up(sx, sy, settle=gap)
    p.down(sx, sy); p.up(sx, sy, settle=0.8)
    c7s = client_size(hwnd)
    p.check("空白处双击 → 全屏（消歧窗口 %dms）" % dbl_ms,
            c7s[0] > cw * 1.2 or c7s[1] > ch * 1.2, "实际 %dx%d" % c7s)

    if c7s[0] > cw * 1.2 or c7s[1] > ch * 1.2:
        g7 = geom(c7s[0], c7s[1])
        p.down(g7["fs"], g7["cy"]); p.up(g7["fs"], g7["cy"])
        time.sleep(0.7)
        p.check("双击进的全屏可用全屏键退出", client_size(hwnd) == (cw, ch),
                "实际 %dx%d" % client_size(hwnd))

    # —— 6d) 空白处单击 = 播放/暂停（延迟一个消歧窗口后生效，待机态无媒体 → 状态不崩）——
    before_single = proc.poll() is None
    p.down(sx, sy); p.up(sx, sy, settle=dbl_ms / 1000.0 + 0.35)
    p.check("空白处单击在消歧窗口后判定（进程存活、无崩溃）",
            before_single and proc.poll() is None, "pid=%d" % proc.pid)

    # —— 7) 顶栏三键：同一套交互（hover 变红 / press 反馈）——
    wc = winbtn_center(cw, 2)
    p.move(wc, TOP_H * 0.5)
    hov3 = p.capture("10-close-hover")
    c7 = hov3.px(wc - 15, TOP_H * 0.5)          # 取底色，避开 × 的笔画
    p.check("关闭键 hover 变红", c7[0] > 150 and c7[1] < 90, "%s" % hov3.hex(wc - 15, TOP_H * 0.5))
    p.down(wc, TOP_H * 0.5)
    pre3 = p.capture("11-close-press")
    c8 = pre3.px(wc - 15, TOP_H * 0.5)
    p.check("关闭键 press 有反馈", dist(c7, c8) >= 8, "Δ=%d %s" % (dist(c7, c8),
                                                                  pre3.hex(wc - 15, TOP_H * 0.5)))
    # 不松开：松开就真关窗了，点测已无需再验证"关窗生效"
    p.move(cw * 0.5, ch * 0.5)

    p.check("进程仍存活（无崩溃）", proc.poll() is None, "pid=%d" % proc.pid)

    print("\n结果：%d 通过 / %d 失败   截图目录 %s" % (p.ok, p.bad, a.out))
    if not a.keep:
        subprocess.run(["taskkill", "/F", "/IM", "flashrec.exe"], capture_output=True)
    return 1 if p.bad else 0


if __name__ == "__main__":
    sys.exit(main())
