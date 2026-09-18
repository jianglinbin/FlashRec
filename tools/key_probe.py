import ctypes, time, sys
u = ctypes.windll.user32
hwnd = u.FindWindowW(None, "FlashRec 投屏接收")
assert hwnd, "window not found"
WM_KEYDOWN, WM_KEYUP = 0x100, 0x101
VK = {"RIGHT": 0x27, "LEFT": 0x25, "UP": 0x26, "DOWN": 0x28}
name = sys.argv[1].upper() if len(sys.argv) > 1 else "RIGHT"
rep = int(sys.argv[2]) if len(sys.argv) > 2 else 2
if len(sys.argv) > 3 and sys.argv[3] == "hold":
    # 模拟按住：1 次 DOWN + rep 次 REPEAT（GLFW 按"键已按下"判 REPEAT）
    u.PostMessageW(hwnd, WM_KEYDOWN, VK[name], 0)
    time.sleep(0.08)
    for i in range(rep):
        u.PostMessageW(hwnd, WM_KEYDOWN, VK[name], 0)
        time.sleep(0.08)
    u.PostMessageW(hwnd, WM_KEYUP, VK[name], 0xC0000000)
else:
    for i in range(rep + 1):
        u.PostMessageW(hwnd, WM_KEYDOWN, VK[name], 0)
        time.sleep(0.05)
        u.PostMessageW(hwnd, WM_KEYUP, VK[name], 0xC0000000)
        time.sleep(0.05)
print(f"已发送 {name} x{rep+1} -> hwnd={hwnd}")

