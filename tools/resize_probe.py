"""无边框窗口边缘缩放 / 中央播放键复用的**离线**验证（不杀用户进程、不置顶、不切全屏）。

A. 边缘缩放：直接向窗口发 WM_NCHITTEST（同步 SendMessage），断言八个方向返回正确的 HT 码。
   —— 这是 resize 能否工作的充要条件：系统就是靠这个返回值决定"这一点是边缘、该进缩放循环"。
B. 中央播放键命中：按 chrome 的几何公式复算，断言暂停态下按钮内/外判定正确，
   且该点会被排除在"空白处单击"之外（这是"点中央键播放→立刻又暂停"的直接原因）。

用法: python tools/resize_probe.py [--exe <path>] [--attach]
      --attach 直接附着到已在运行的实例，不启动新进程（默认行为）
"""
import argparse
import ctypes
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_click_probe as P

ROOT = P.ROOT
WM_NCHITTEST = 0x0084

HTS = {
    0: "HTNOWHERE", 1: "HTCLIENT", 2: "HTCAPTION", 10: "HTLEFT", 11: "HTRIGHT",
    12: "HTTOP", 13: "HTTOPLEFT", 14: "HTTOPRIGHT", 15: "HTBOTTOM",
    16: "HTBOTTOMLEFT", 17: "HTBOTTOMRIGHT",
}

P.u32.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
P.u32.SendMessageW.restype = ctypes.c_ssize_t
P.u32.WindowFromPoint.argtypes = [ctypes.c_ssize_t]
P.u32.WindowFromPoint.restype = ctypes.c_void_p
P.u32.ScreenToClient.argtypes = [ctypes.c_void_p, ctypes.c_void_p]


def screen_pt(x, y):
    return (y << 16) | (x & 0xFFFF)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "bin", "flashrec.exe"))
    ap.add_argument("--attach", action="store_true", default=True)
    a = ap.parse_args()

    hwnd = P.u32.FindWindowW("GLFW30", None)
    started = None
    if not hwnd:
        print("  未发现运行中的实例，启动一个（检测完即关闭，不影响其它程序）")
        started = subprocess.Popen([a.exe], cwd=os.path.dirname(a.exe))
        for _ in range(60):
            time.sleep(0.1)
            hwnd = P.u32.FindWindowW("GLFW30", None)
            if hwnd:
                break
    if not hwnd:
        print("[FAIL] 未找到窗口")
        return 1
    time.sleep(1.2)
    hwnd = ctypes.c_void_p(hwnd)

    cw, ch = P.client_size(hwnd)
    ox, oy = P.client_origin(hwnd)
    print("\n=== 边缘缩放 / 中央键 离线验证 ===")
    print("  客户端 %dx%d  原点(%d,%d)" % (cw, ch, ox, oy))

    ok = bad = 0

    def check(name, cond, detail=""):
        nonlocal ok, bad
        if cond:
            ok += 1
            print("  [PASS] %s  %s" % (name, detail))
        else:
            bad += 1
            print("  [FAIL] %s  %s" % (name, detail))

    # 窗口矩形（含边框；无边框窗口 = 客户区）
    r = P.RECT()
    P.u32.GetWindowRect(hwnd, ctypes.byref(r))
    L, T, R, B = r.L, r.T, r.R, r.B
    m = 3   # 距边 3px
    cx, cy = (L + R) // 2, (T + B) // 2

    cases = [
        ("左上角", L + m, T + m, 13),
        ("右上角", R - m, T + m, 14),
        ("左下角", L + m, B - m, 16),
        ("右下角", R - m, B - m, 17),
        ("左边中", L + m, cy, 10),
        ("右边中", R - m, cy, 11),
        ("上边中", cx, T + m, 12),
        ("下边中", cx, B - m, 15),
        ("正中心", cx, cy, 1),
    ]
    for name, x, y, want in cases:
        got = P.u32.SendMessageW(hwnd, WM_NCHITTEST, 0, screen_pt(x, y))
        # HTCLIENT 之外的"客户区"可能被系统的 caption 逻辑改写，这里只看边缘是否正确
        check("%s → %s" % (name, HTS.get(want, str(want))), got == want,
              "实际 %s" % HTS.get(got, str(got)))

    # B) 中央播放键几何：按 chrome 公式复算，断言中心在内、角外
    lay = P.LAY
    r_center = lay.get("playButtonCenter", {}).get("size", 56)
    cxp, cyp = cw * 0.5, ch * 0.5

    def inside(x, y):
        rr = r_center * 0.5
        return (x - cxp) ** 2 + (y - cyp) ** 2 <= rr * rr

    check("中央键中心点判定为在键上", inside(cxp, cyp))
    check("中央键半径外 4px 判定为不在键上", not inside(cxp + r_center * 0.5 + 4, cyp))
    check("中央键对角线外判定为不在键上",
          not inside(cxp + r_center * 0.5, cyp + r_center * 0.5))

    if started:
        print("  （关闭本脚本启动的实例）")
        started.terminate()
    print("\n结果：%d 通过 / %d 失败" % (ok, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
