"""只读探测：验证窗口角落 45° 双边拖拽区已被扩大（kCornerBand = 3 * kResizeBand）。

对 flashrec 主窗口（class='GLFW30'）逐点发 WM_NCHITTEST，打印返回码。
重点看：距角 ~12px 处、只贴一条边的点，现在应返回对角缩放码（13/14/16/17），
而不再是单纯的 HTLEFT/HTRIGHT/HTTOP/HTBOTTOM（10/11/12/15）。

不杀进程、不改窗口状态。
"""
import ctypes
import sys

u32 = ctypes.windll.user32

for fn in (
    lambda: u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)),
    lambda: ctypes.windll.shcore.SetProcessDpiAwareness(2),
    lambda: ctypes.windll.kernel32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)),
):
    try:
        fn()
    except Exception:
        pass

HT = {
    0: "HTNOWHERE", 1: "HTCLIENT", 2: "HTCAPTION", 3: "HTSYSMENU", 4: "HTGROWBOX",
    10: "HTLEFT", 11: "HTRIGHT", 12: "HTTOP", 13: "HTTOPLEFT", 14: "HTTOPRIGHT",
    15: "HTBOTTOM", 16: "HTBOTTOMLEFT", 17: "HTBOTTOMRIGHT",
}


def find_main():
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(hwnd, _):
        buf = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(hwnd, buf, 256)
        if buf.value == "GLFW30":
            r = ctypes.wintypes.RECT()
            u32.GetWindowRect(hwnd, ctypes.byref(r))
            if u32.IsWindowVisible(hwnd) and r.right - r.left > 100:
                found.append(hwnd)
        return True

    u32.EnumWindows(cb, None)
    return found[0] if found else None


def main():
    hwnd = find_main()
    if not hwnd:
        print("未找到 flashrec 主窗口（class=GLFW30）")
        return 1
    r = ctypes.wintypes.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    print(f"主窗口 HWND={hwnd:#x} 尺寸 {w}x{h}")

    # (描述, 相对窗口的 x, y)；y 从上往下
    pts = [
        ("左上 距角 4", 4, 4),
        ("左上 距角 12", 12, 12),
        ("左上 距角 17", 17, 17),
        ("左上 距角 20 (越界)", 20, 20),
        ("上边 距左边 12", 12, 3),
        ("上边 距左边 17", 17, 3),
        ("上边 距左边 20 (应回 HTTOP)", 20, 3),
        ("左边 距上边 12", 3, 12),
        ("左边 距上边 17", 3, 17),
        ("左边 距上边 20 (应回 HTLEFT)", 3, 20),
        ("上边 正中", w // 2, 3),
        ("左边 正中", 3, h // 2),
        ("右下 距角 12", w - 12, h - 12),
        ("右边 距下边 12", w - 3, h - 12),
        ("下边 距右边 12", w - 12, h - 3),
        ("右下 距角 20 (越界)", w - 20, h - 20),
    ]
    for label, x, y in pts:
        lp = (y << 16) | (x & 0xFFFF)
        code = u32.SendMessageW(hwnd, 0x0084, 0, lp)  # WM_NCHITTEST
        name = HT.get(code, str(code))
        mark = ""
        if code in (13, 14, 16, 17):
            mark = "  <== 对角（双边）"
        print(f"  {label:<26} ({x:>4},{y:>4}) -> {code:>3} {name}{mark}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
