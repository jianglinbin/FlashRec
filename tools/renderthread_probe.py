"""验证渲染线程的独立性：统计模态循环期间渲染线程是否仍在出帧。

判据：用 GPU 引擎占用 + 进程 CPU 时间增量间接判断。
更直接的办法：GLFW 的 wait_events_timeout(0.008) 让渲染线程即使空闲也在跑，
所以比较"空闲期"与"模态循环期"的进程 CPU 时间增速即可 —— 若渲染线程被主线程
的模态循环拖住，模态期间 CPU 增速会显著下降（趋近于只做消息泵）。

注意：这是间接测量，只用于发现"渲染完全停滞"这种量级的差异。
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
import time

u32 = ctypes.windll.user32
for _fn in (lambda: u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)),
            lambda: ctypes.windll.shcore.SetProcessDpiAwareness(2)):
    try:
        _fn()
    except Exception:
        pass

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
k32 = ctypes.windll.kernel32

WM_SYSCOMMAND = 0x0112
SC_SIZE = 0xF000
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
VK_ESCAPE = 0x1B
WM_MOUSEMOVE = 0x0200


class FILETIME(ctypes.Structure):
    _fields_ = [("dwLowDateTime", wt.DWORD), ("dwHighDateTime", wt.DWORD)]


def to_int(ft):
    return (ft.dwHighDateTime << 32) | ft.dwLowDateTime


def cpu_time(hpid):
    h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, hpid)
    if not h:
        return None
    c, e, k, u = FILETIME(), FILETIME(), FILETIME(), FILETIME()
    ok = k32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e),
                             ctypes.byref(k), ctypes.byref(u))
    k32.CloseHandle(h)
    if not ok:
        return None
    return to_int(k) + to_int(u)


def find_main_hwnd():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq flashrec.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    pid = None
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].lower() == "flashrec.exe":
            pid = int(parts[1])
    if pid is None:
        return None, None
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
    return pid, (hits[0] if hits else None)


def sample_cpu(pid, seconds, label):
    t0 = cpu_time(pid)
    if t0 is None:
        print("  无法读取 CPU 时间")
        return None
    time.sleep(seconds)
    t1 = cpu_time(pid)
    if t1 is None:
        return None
    # FILETIME 单位 100ns
    pct = (t1 - t0) / (seconds * 1e7) * 100.0
    print(f"  {label}: CPU 占用 {pct:5.1f}%  ({seconds}s 采样)")
    return pct


def main():
    pid, hwnd = find_main_hwnd()
    if not hwnd:
        print("[SKIP] 未找到运行中的 flashrec 窗口")
        return 2
    print(f"pid={pid} hwnd=0x{hwnd:X}")

    u32.SetForegroundWindow(hwnd)
    time.sleep(0.5)
    print("\n[1] 空闲基线（无操作）")
    base = sample_cpu(pid, 2.0, "空闲")

    print("\n[2] 进入系统模态循环（等价用户拖拽边缘）")
    u32.PostMessageW(hwnd, WM_SYSCOMMAND, SC_SIZE, 0)
    time.sleep(0.3)
    modal = sample_cpu(pid, 2.0, "模态中")

    print("\n[3] 退出模态循环")
    u32.PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0)
    u32.PostMessageW(hwnd, WM_KEYUP, VK_ESCAPE, 0)
    time.sleep(0.5)
    after = sample_cpu(pid, 2.0, "退出后")

    if base is None or modal is None:
        return 1
    print("\n=== 判读 ===")
    ratio = modal / base if base > 0.01 else float("inf")
    print(f"模态中/空闲 CPU 比 = {ratio:.2f}")
    if ratio < 0.35:
        print(">>> 疑似渲染线程被拖住（模态期间 CPU 显著下降）")
        return 1
    print(">>> 渲染线程在模态期间持续工作（CPU 未塌陷）→ 线程化生效")
    return 0


if __name__ == "__main__":
    sys.exit(main())
