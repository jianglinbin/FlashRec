# FlashRec 程序图标生成（d68 §6）
# 产出：assets/icons/app_{16,32,48,256}.png（RGBA，任务栏 / Alt-Tab / X11 _NET_WM_ICON）
# 设计：深板岩圆角方底 + 白色"显示器"描边 + 青色投屏波（接收方向），16px 仍可辨识。
# 全程 1024px 超采样绘制 → LANCZOS 降采样，保证小尺寸边缘干净。
# 运行：python tools/make_icon.py
from PIL import Image, ImageDraw

SS = 1024  # 超采样画布


def rounded_mask(size, inset, radius):
    m = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle([inset, inset, size - inset, size - inset], radius=radius, fill=255)
    return m


def build():
    img = Image.new("RGBA", (SS, SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    bg = (27, 35, 48, 255)        # 深板岩
    screen = (233, 238, 246, 255)  # 显示器描边（近白）
    wave = (58, 194, 206, 255)     # 投屏波（青）

    # —— 底板：圆角方 ——
    d.rounded_rectangle([0, 0, SS, SS], radius=224, fill=bg)
    # 顶部 1px 高光内描边（可选细节，256px 下有质感，小尺寸自动糊掉无碍）
    d.rounded_rectangle([6, 6, SS - 6, SS - 6], radius=218, outline=(255, 255, 255, 18), width=4)

    # —— 显示器描边 ——
    sx0, sy0, sx1, sy1 = 168, 232, 856, 792
    d.rounded_rectangle([sx0, sy0, sx1, sy1], radius=56, outline=screen, width=52)

    # —— 投屏波（接收）：左下角圆点 + 三道四分之一弧（朝右上展开）——
    cx, cy = 336, 648  # 圆心（屏幕内左下）
    dot_r = 40
    d.ellipse([cx - dot_r, cy - dot_r, cx + dot_r, cy + dot_r], fill=wave)
    for r, w in ((150, 52), (262, 52), (374, 52)):
        bbox = [cx - r, cy - r, cx + r, cy + r]
        d.arc(bbox, start=-90, end=0, fill=wave, width=w)

    return img


def main():
    base = build()
    for size in (256, 48, 32, 16):
        out = base.resize((size, size), Image.LANCZOS)
        path = f"assets/icons/app_{size}.png"
        out.save(path)
        print("written", path)
    # Windows exe 图标（多尺寸 ICO）：http 资源编译进 exe，桌面/开始菜单/任务栏快捷方式取它。
    # 没有它，exe 无图标资源 → 快捷方式显示成"通用文档"（实测踩过）。
    ico = "assets/icons/app.ico"
    base.save(ico, format="ICO",
              sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("written", ico)


if __name__ == "__main__":
    main()
