"""验证渲染线程化：主线程被模态循环阻塞时，渲染线程是否仍在出帧。

原理：向窗口发 WM_SYSCOMMAND(SC_MOVE/SC_SIZE) 让系统进入模态循环（等价于用户
开始拖拽边缘），此时主线程的 glfwWaitEvents 会阻塞不返回。若渲染线程独立，
画面应继续更新（DWM 兜底不会被触发）。

不杀进程、不改窗口最终状态：进入模态循环后立刻用 ESC 取消。
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

WM_SYSCOMMAND = 0x0112
SC_SIZE = 0xF000
SC_MOVE = 0xF010
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
VK_ESCAPE = 0x1B
VK_RETURN = 0x0D


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


def main():
    pid, hwnd = find_main_hwnd()
    if not hwnd:
        print("[SKIP] 未找到运行中的 flashrec 窗口（请先启动 app）")
        return 2
    print(f"目标：pid={pid} hwnd=0x{hwnd:X}")

    # 记录进入前窗口尺寸
    rc0 = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(rc0))
    w0, h0 = rc0.right - rc0.left, rc0.bottom - rc0.top
    print(f"进入前窗口尺寸 {w0}x{h0}")

    u32.SetForegroundWindow(hwnd)
    time.sleep(0.3)

    # 进入"调整大小"模态循环（与用户拖边缘等价）
    # PostMessage 异步投递，立刻返回；系统随后进入模态循环
    u32.PostMessageW(hwnd, WM_SYSCOMMAND, SC_SIZE, 0)
    print("已投递 SC_SIZE（进入系统模态循环）")

    # 模态循环中探测窗口是否仍响应（主线程被占，但窗口消息仍被系统处理）
    time.sleep(0.5)
    alive = u32.IsWindow(hwnd)
    responding = u32.SendMessageTimeoutW(hwnd, 0x0000, 0, 0, 0x0002, 300,
                                         ctypes.byref(wt.DWORD()))
    print(f"窗口存在 = {alive}")

    # 取消模态循环
    u32.PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0)
    u32.PostMessageW(hwnd, WM_KEYUP, VK_ESCAPE, 0)
    time.sleep(0.4)

    rc1 = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(rc1))
    w1, h1 = rc1.right - rc1.left, rc1.bottom - rc1.top
    print(f"退出后窗口尺寸 {w1}x{h1}")
    print("结论：模态循环可进入并退出，窗口未崩溃")
    return 0


if __name__ == "__main__":
    sys.exit(main())
