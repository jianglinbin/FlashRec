#!/usr/bin/env python3
"""列出 FlashRec 进程/窗口的 DPI 相关状态，用于诊断"渲染尺寸与窗口尺寸不一致"。

打印：窗口外框(物理) / 客户区(物理) / 客户区(逻辑) / DPI / 进程 DPI 感知级别。
只读，不修改任何状态。
"""
import ctypes
from ctypes import wintypes

# 必须在任何窗口 API 之前：否则本脚本自身被 DPI 虚拟化，读到的尺寸无参考价值。
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)  # PER_MONITOR_DPI_AWARE
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

u = ctypes.windll.user32
shcore = ctypes.windll.shcore

TITLE = "FlashRec 投屏接收"

DPI_AWARENESS_CONTEXT_UNAWARE = -1
DPI_AWARENESS_CONTEXT_SYSTEM_AWARE = -2
DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE = -3
DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED = -5

NAMES = {
    DPI_AWARENESS_CONTEXT_UNAWARE: "UNAWARE",
    DPI_AWARENESS_CONTEXT_SYSTEM_AWARE: "SYSTEM_AWARE",
    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE: "PER_MONITOR_AWARE",
    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2: "PER_MONITOR_AWARE_V2",
    DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED: "UNAWARE_GDISCALED",
}


def main():
    hwnd = u.FindWindowW(None, TITLE)
    if not hwnd:
        print("未找到 FlashRec 窗口")
        return 1

    # 进程 DPI 感知（需 Win10 1703+；失败则回退 shcore）
    try:
        ctx = u.GetThreadDpiAwarenessContext()
        # 用 AreDpiAwarenessContextsEqual 比对
        def eq(a, b):
            return bool(u.AreDpiAwarenessContextsEqual(a, b))
        level = "未知"
        for v, n in NAMES.items():
            if eq(ctx, ctypes.c_void_p(v)):
                level = n
                break
    except Exception:
        level = "GetThreadDpiAwarenessContext 不可用"

    # 客户区（物理 = 与 DPI 感知一致；GetClientRect 本身返回逻辑/物理取决于感知级别）
    cr = wintypes.RECT()
    u.GetClientRect(hwnd, ctypes.byref(cr))
    wr = wintypes.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(wr))

    dpi = u.GetDpiForWindow(hwnd) if hasattr(u, "GetDpiForWindow") else 96
    scale = dpi / 96.0

    print(f"窗口标题      : {TITLE}")
    print(f"hwnd          : 0x{hwnd:X}")
    print(f"进程 DPI 感知 : {level}")
    print(f"窗口 DPI      : {dpi}  (缩放 {scale:.3f}x)")
    print(f"窗口外框      : {wr.right - wr.left} x {wr.bottom - wr.top}  (物理像素)")
    print(f"客户区        : {cr.right - cr.left} x {cr.bottom - cr.top}")
    print(f"  按 DPI 折算 : 物理 = {int((cr.right - cr.left) * scale)} x "
          f"{int((cr.bottom - cr.top) * scale)}")
    print()
    print("判读：若『客户区』≈ 外框，说明进程是 DPI-aware（客户区已是物理像素）；")
    print("      若『客户区 × 缩放』≈ 外框，说明客户区是逻辑像素（进程不感知或未映射）。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
