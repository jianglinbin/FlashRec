#!/usr/bin/env python3
"""让运行中的 flashrec 实例**走正常退出路径**关闭。

Windows（d117 修订）：
  首选注册消息 "FlashRec.RequestQuit"（RegisterWindowMessageW 按消息名跨进程对齐，
  应用 wndproc 拦截后走托盘菜单「退出」同款路径：隐窗 + 置 should_close → 完整收尾链）。
  旧通道为何失效：d81 托盘后 WM_CLOSE = 隐到托盘（回调"已处理"清回 should_close）；
  GLFW 对 WM_QUIT 的处理也是对全部窗口发 close-request（third_party/glfw
  win32_window.c:2109），同样被该回调吞掉 —— 二者均无法真退出，仅作兜底保留。
  强杀禁止：会跳过 GL 资源释放与 mpv 注销，也会让后续构建撞 LNK1104。

Linux（d88 补）：
  ELF 无"文件被占用"问题，且 Wayland 会话下没有 X 客户端消息通道可用 → 用 SIGTERM。
  找进程必须精确匹配进程名（pgrep -x / 扫 /proc/*/comm）：pgrep -f 会匹配到调用者
  自己的命令行（历史踩坑：pkill -f 自杀）。

用法:
  python tools/quit_app.py            # 找到就退，没找到就静默返回 0
  python tools/quit_app.py --wait     # 退掉后等进程真正消失再返回
"""
import os
import subprocess
import sys
import time

PROC_NAME = "flashrec"


# --------------------------------------------------------------------------- #
# Linux                                                                       #
# --------------------------------------------------------------------------- #
def linux_pids():
    """运行中的 flashrec pid 列表（pgrep 优先，失败则扫 /proc）。"""
    try:
        r = subprocess.run(["pgrep", "-x", PROC_NAME], capture_output=True, text=True)
        if r.returncode == 0:
            return [int(x) for x in r.stdout.split() if x.strip()]
        if r.returncode == 1:      # 未找到（而非 pgrep 不可用）
            return []
    except OSError:
        pass
    pids = []
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            with open("/proc/%s/comm" % name) as f:
                if f.read().strip() == PROC_NAME:
                    pids.append(int(name))
        except OSError:
            continue
    return pids


def linux_main(wait):
    import signal

    pids = linux_pids()
    if not pids:
        print("没有运行中的 flashrec 进程")
        return 0
    for pid in pids:
        try:
            os.kill(pid, signal.SIGTERM)
            print("已发送 SIGTERM → pid=%d" % pid)
        except ProcessLookupError:
            pass
    if wait:
        for _ in range(50):        # 最多等 5s
            if not linux_pids():
                print("进程已退出")
                return 0
            time.sleep(0.1)
        print("警告：等待 5s 后进程仍在（可能阻塞在收尾）")
        return 1
    return 0


# --------------------------------------------------------------------------- #
# Windows                                                                     #
# --------------------------------------------------------------------------- #
WM_CLOSE = 0x0010
WM_QUIT = 0x0012
TH32CS_SNAPTHREAD = 0x4


def win_main(wait):  # pragma: no cover - 仅在 Windows 上执行
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32

    def find_windows():
        found = []

        @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        def cb(hwnd, _lp):
            n = user32.GetWindowTextLengthW(hwnd)
            if n > 0:
                buf = ctypes.create_unicode_buffer(n + 1)
                user32.GetWindowTextW(hwnd, buf, n + 1)
                if "FlashRec" in buf.value:
                    found.append((hwnd, buf.value))
            return True

        user32.EnumWindows(cb, 0)
        return found

    def post_quit_message(hwnd):
        """d117：发注册消息 FlashRec.RequestQuit（首选，走完整收尾链）。"""
        msg = user32.RegisterWindowMessageW(ctypes.c_wchar_p("FlashRec.RequestQuit"))
        user32.PostMessageW(hwnd, msg, 0, 0)

    def post_quit(hwnd):
        """向创建该窗口的线程（= GLFW 主线程）投 WM_QUIT；GLFW poll 置 should_close。"""
        pid = wintypes.DWORD(0)
        tid = user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))

        class THREADENTRY32(ctypes.Structure):
            _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                        ("th32ThreadID", wintypes.DWORD), ("th32OwnerProcessID", wintypes.DWORD),
                        ("tpBasePri", wintypes.LONG), ("tpDeltaPri", wintypes.LONG),
                        ("dwFlags", wintypes.DWORD)]

        snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
        target = tid  # 窗口创建线程即 GLFW 主线程，直接投；快照遍历仅作兜底
        if snap != -1:
            entry = THREADENTRY32()
            entry.dwSize = ctypes.sizeof(entry)
            ok = kernel32.Thread32First(snap, ctypes.byref(entry))
            while ok:
                if entry.th32OwnerProcessID == pid.value:
                    user32.PostThreadMessageW(entry.th32ThreadID, WM_QUIT, 0, 0)
                ok = kernel32.Thread32Next(snap, ctypes.byref(entry))
            kernel32.CloseHandle(snap)
        if target:
            user32.PostThreadMessageW(target, WM_QUIT, 0, 0)

    wins = find_windows()
    if not wins:
        print("没有运行中的 flashrec 窗口")
        return 0
    for hwnd, title in wins:
        # d117：首选注册消息（托盘语义吞不掉，走「退出」同款完整收尾链）
        post_quit_message(hwnd)
        print("已发送 RequestQuit 注册消息 → %r (hwnd=%s)" % (title, hwnd))
    time.sleep(1.0)
    remaining = find_windows()
    if remaining:
        # 兜底（旧版本应用未实现注册消息时）：WM_CLOSE + WM_QUIT
        for hwnd, title in remaining:
            user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
            print("已兜底发送 WM_CLOSE → %r (hwnd=%s)" % (title, hwnd))
            post_quit(hwnd)
            print("已兜底补发 WM_QUIT（托盘模式）→ %r" % (title,))
    if wait:
        for _ in range(50):        # 最多等 5s
            if not find_windows():
                print("进程已退出")
                return 0
            time.sleep(0.1)
        print("警告：等待 5s 后窗口仍在（可能阻塞在收尾）")
        return 1
    return 0


def main():
    wait = "--wait" in sys.argv
    if sys.platform.startswith("linux"):
        return linux_main(wait)
    return win_main(wait)


if __name__ == "__main__":
    sys.exit(main())
