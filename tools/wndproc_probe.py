"""诊断：检查 flashrec 主窗口的 WNDPROC 是否被子类化、WM_NCHITTEST 是否返回缩放码。

只读探测，不杀进程、不改窗口状态。用 live 进程的真实 HWND 发 WM_NCHITTEST。
"""
import ctypes
import ctypes.wintypes as wt
import sys

u32 = ctypes.windll.user32

# 让本进程 DPI 感知，否则坐标被虚拟化
try:
    ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        pass

k32 = ctypes.windll.kernel32
try:
    k32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass


def find_windows():
    """列出所有属于 flashrec.exe 的顶层窗口"""
    pid = None
    import subprocess
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq flashrec.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].lower() == "flashrec.exe":
            pid = int(parts[1])
    if pid is None:
        print("未找到 flashrec.exe 进程")
        return None, []

    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def enum_cb(hwnd, _):
        wpid = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value == pid:
            cls = ctypes.create_unicode_buffer(256)
            u32.GetClassNameW(hwnd, cls, 256)
            title = ctypes.create_unicode_buffer(256)
            u32.GetWindowTextW(hwnd, title, 256)
            found.append((hwnd, cls.value, title.value, bool(u32.IsWindowVisible(hwnd))))
        return True

    u32.EnumWindows(enum_cb, 0)
    return pid, found


# WM_NCHITTEST 的返回码名称
HT = {
    -1: "HTTRANSPARENT", 0: "HTNOWHERE", 1: "HTCLIENT", 2: "HTCAPTION",
    3: "HTSYSMENU", 4: "HTGROWBOX", 5: "HTMENU", 6: "HTHSCROLL", 7: "HTVSCROLL",
    8: "HTMINBUTTON", 9: "HTMAXBUTTON", 10: "HTLEFT", 11: "HTRIGHT",
    12: "HTTOP", 13: "HTTOPLEFT", 14: "HTTOPRIGHT", 15: "HTBOTTOM",
    16: "HTBOTTOMLEFT", 17: "HTBOTTOMRIGHT", 18: "HTBORDER", 20: "HTCLOSE",
    21: "HTHELP",
}

WM_NCHITTEST = 0x0084


def main():
    pid, wins = find_windows()
    print(f"flashrec.exe pid = {pid}")
    if not wins:
        print("没有枚举到属于该进程的窗口")
        return 1

    for hwnd, cls, title, vis in wins:
        print(f"\n窗口 HWND=0x{hwnd:X}  class={cls!r}  title={title!r}  visible={vis}")
        rc = wt.RECT()
        u32.GetWindowRect(hwnd, ctypes.byref(rc))
        w, h = rc.right - rc.left, rc.bottom - rc.top
        print(f"  窗口矩形 = ({rc.left},{rc.top}) - ({rc.right},{rc.bottom})  尺寸 {w}x{h}")
        if w <= 0 or h <= 0:
            continue

        u32.SetForegroundWindow(hwnd)

        # 探针点（屏幕坐标）：四角内 3px、四边中内 3px、正中心、标题栏中间
        pts = {
            "左上 (3,3)":          (rc.left + 3, rc.top + 3),
            "右上 (w-3,3)":        (rc.right - 3, rc.top + 3),
            "左下 (3,h-3)":        (rc.left + 3, rc.bottom - 3),
            "右下 (w-3,h-3)":      (rc.right - 3, rc.bottom - 3),
            "左边中 (3,h/2)":      (rc.left + 3, rc.top + h // 2),
            "右边中 (w-3,h/2)":    (rc.right - 3, rc.top + h // 2),
            "上边中 (w/2,3)":      (rc.left + w // 2, rc.top + 3),
            "下边中 (w/2,h-3)":    (rc.left + w // 2, rc.bottom - 3),
            "正中心":              (rc.left + w // 2, rc.top + h // 2),
            "边外 12px (12,3)":    (rc.left + 12, rc.top + 3),
        }
        print("  WM_NCHITTEST 探测：")
        for name, (sx, sy) in pts.items():
            lp = ((sy & 0xFFFF) << 16) | (sx & 0xFFFF)
            r = u32.SendMessageW(hwnd, WM_NCHITTEST, 0, lp)
            ok = "  <== 缩放" if r in (10, 11, 12, 13, 14, 15, 16, 17) else ""
            print(f"    {name:<20} -> {int(r):>3} {HT.get(int(r), '?'):<14}{ok}")

        # 直接看窗口过程的地址，判断子类化是否生效
        GWLP_WNDPROC = -4
        cur = u32.GetWindowLongPtrW(hwnd, GWLP_WNDPROC)
        print(f"  当前 WNDPROC 地址 = 0x{cur:X}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
