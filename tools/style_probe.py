"""诊断：检查 flashrec 主窗口的 WS_ 样式，确认是否含 WS_THICKFRAME（边缘缩放的前提）。

无边框窗口（DECORATED=false）通常没有 WS_THICKFRAME，此时 WM_NCHITTEST 虽然返回
HTLEFT 等码，但 DefWindowProc 在 WM_NCLBUTTONDOWN 时找不到 thick frame，不会进入
缩放循环 —— 这就是"命中测试正确却拖不动"的根因。
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys

u32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32
for _fn in (lambda: u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)),
            lambda: ctypes.windll.shcore.SetProcessDpiAwareness(2),
            lambda: k32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))):
    try:
        _fn()
    except Exception:
        pass


def main_hwnd():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq flashrec.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    pid = None
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].lower() == "flashrec.exe":
            pid = int(parts[1])
    if pid is None:
        print("未找到 flashrec.exe 进程")
        return None
    hits = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        wpid = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value == pid:
            cls = ctypes.create_unicode_buffer(64)
            u32.GetClassNameW(hwnd, cls, 64)
            if cls.value == "GLFW30":
                hits.append(hwnd)
        return True

    u32.EnumWindows(cb, 0)
    return hits[0] if hits else None


GWL_STYLE, GWL_EXSTYLE = -16, -20
WS = [("WS_POPUP", 0x80000000), ("WS_CHILD", 0x40000000), ("WS_MINIMIZE", 0x20000000),
      ("WS_VISIBLE", 0x10000000), ("WS_DISABLED", 0x08000000), ("WS_CLIPSIBLINGS", 0x04000000),
      ("WS_CLIPCHILDREN", 0x02000000), ("WS_MAXIMIZE", 0x01000000), ("WS_BORDER", 0x00800000),
      ("WS_DLGFRAME", 0x00400000), ("WS_VSCROLL", 0x00200000), ("WS_HSCROLL", 0x00100000),
      ("WS_SYSMENU", 0x00080000), ("WS_THICKFRAME", 0x00040000), ("WS_GROUP", 0x00020000),
      ("WS_MINIMIZEBOX", 0x00020000), ("WS_MAXIMIZEBOX", 0x00010000)]
WSEX = [("WS_EX_DLGMODALFRAME", 0x1), ("WS_EX_TOPMOST", 0x8), ("WS_EX_TRANSPARENT", 0x20),
        ("WS_EX_TOOLWINDOW", 0x80), ("WS_EX_APPWINDOW", 0x40000), ("WS_EX_LAYERED", 0x80000),
        ("WS_EX_NOACTIVATE", 0x8000000), ("WS_EX_NOREDIRECTIONBITMAP", 0x200000)]


def main():
    hwnd = main_hwnd()
    if not hwnd:
        return 1
    style = u32.GetWindowLongW(hwnd, GWL_STYLE)
    ex = u32.GetWindowLongW(hwnd, GWL_EXSTYLE)
    print(f"HWND = 0x{hwnd:X}")
    print(f"style = 0x{style & 0xFFFFFFFF:08X}   exstyle = 0x{ex & 0xFFFFFFFF:08X}\n")

    print("--- 已设置的 WS_ 样式 ---")
    for n, v in WS:
        if style & v:
            print(f"  [+] {n}")
    print("\n--- 缩放相关的关键位 ---")
    for n, v in [("WS_THICKFRAME (可调整边框)", 0x00040000),
                 ("WS_CAPTION", 0x00C00000),
                 ("WS_SYSMENU", 0x00080000),
                 ("WS_MAXIMIZEBOX", 0x00010000),
                 ("WS_MINIMIZEBOX", 0x00020000)]:
        print(f"  {'[有]' if style & v else '[缺]'} {n}")
    print("\n--- 已设置的 WS_EX_ 样式 ---")
    for n, v in WSEX:
        if ex & v:
            print(f"  [+] {n}")
    print(f"\nIsZoomed(最大化)  = {bool(u32.IsZoomed(hwnd))}")
    print(f"IsIconic(最小化)  = {bool(u32.IsIconic(hwnd))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
