#!/usr/bin/env python3
"""离线验证「FBO 贴图必须不平移、不翻转」这条铁律（不起 GL、不跑 mpv）。

背景（d17 → d18 连续踩坑）：
  把 mpv 的 FBO 纹理贴到窗口时，这段代码先后试过三种翻转写法，**全是错的**：

    a. create_image_from_handle(..., NVG_IMAGE_FLIPY)
       → 镜像。该分支的枢轴取自 frag->extent[1] * 0.5f（nanovg_gl.h），
         隐含要求 "pattern 的 h == 注册图像高"，条件一变即静默失效。
    b. nvgImagePattern(cy = +H, h = -H)
       → 镜像。nanovg 内部对 paint->xform 取**逆**后再送进 shader
         （nvgTransformInverse → glnvg__xformToMat3x4），故
             pt.y = (screen.y - cy) / h = 1 - y/H
         恰好是上下翻。
    c. nvgImagePattern(cy = -H, h = -H)
       → 下半屏糊成上半屏的复制。pt.y = -y/H - 1 ∈ [-2,-1]，全在纹理外。

  正确做法：**什么都不做** —— cx = cy = 0，extent 取正的 (W, H)，flags = 0。
  原因：FBO 纹理的原点在左下，mpv 渲染进去的朝向本来就与 nanovg 的 v 轴约定相容。

本脚本把 nanovg 的采样公式搬过来算，作为**回归护栏**：公式或调用参数一改立刻报错。
公式来源：
  nanovg.c   nvgImagePattern():
      p.xform = rotate(angle);  p.xform[4] = cx;  p.xform[5] = cy;
      p.extent[0] = w;          p.extent[1] = h;
  nanovg_gl.h:
      nvgTransformInverse(invxform, paint->xform);
      glnvg__xformToMat3x4(frag->paintMat, invxform);      ← 送进 shader 的是【逆】
  shader (Image 分支):
      pt = (paintMat * vec3(fpos, 1.0)).xy / extent
⇒ angle = 0 时 paint->xform = T(cx, cy)，其逆 = T(-cx, -cy)，故
      pt.x = (x - cx) / w        pt.y = (y - cy) / h

判定标准（"恒等映射"）：屏幕 (0,0) → pt (0,0)、(W,H) → pt (1,1)，且全程单调不减。
"""
import sys

EPS = 1e-6


def pt_x(x, cx, w):
    # shader 收到的是 paint->xform 的逆：pt.x = (x - cx) / w
    return (x - cx) / w


def pt_y(y, cy, h):
    # 同上：pt.y = (y - cy) / h
    return (y - cy) / h


def check_identity(name, cx, cy, w, h, W, H):
    """判定这组参数是否构成"恒等映射"（不翻、不错位）。"""
    xs = [0.0, W * 0.5, float(W)]
    ys = [0.0, H * 0.5, float(H)]
    px = [pt_x(x, cx, w) for x in xs]
    py = [pt_y(y, cy, h) for y in ys]

    in_range = all(-EPS <= v <= 1 + EPS for v in px + py)
    x_ok = abs(px[0]) < EPS and abs(px[1] - 0.5) < EPS and abs(px[2] - 1) < EPS
    y_ok = abs(py[0]) < EPS and abs(py[1] - 0.5) < EPS and abs(py[2] - 1) < EPS
    ok = in_range and x_ok and y_ok

    print(f"  [{'OK ' if ok else '!! '}] {name}")
    print(f"        cx={cx} cy={cy} w={w} h={h}")
    print(f"        pt.x(0..W) = {['%.3f' % v for v in px]}   （要求 0, .5, 1）")
    print(f"        pt.y(0..H) = {['%.3f' % v for v in py]}   （要求 0, .5, 1）")
    if not ok:
        why = []
        if not in_range:
            why.append("采样值越界（会糊成边缘色/半屏复制）")
        if not x_ok:
            why.append("水平映射非恒等（左右翻或错位）")
        if not y_ok:
            why.append("垂直映射非恒等（上下翻或错位）")
        print(f"        ✗ {'；'.join(why)}")
    return ok


def main():
    W, H = 960.0, 540.0  # 取一个真实的窗口尺寸
    print("FBO 贴图参数验证（模型：nanovg.c + nanovg_gl.h，含「逆变换」这一关键点）")
    print("=" * 66)

    cases = [
        ("✔ 正确：不平移不翻转 (cx=0, cy=0, w=+W, h=+H)", 0.0, 0.0, W, H, True),
        ("✗ 曾踩坑：上下翻 (cy=+H, h=-H)", 0.0, H, W, -H, False),
        ("✗ 曾踩坑：半屏复制 (cy=-H, h=-H)", 0.0, -H, W, -H, False),
        ("✗ 假设错误：左右翻 (w=-W)", 0.0, 0.0, -W, H, False),
    ]

    results = []
    for name, cx, cy, w, h, want in cases:
        ok = check_identity(name, cx, cy, w, h, W, H)
        results.append((name, ok, want))

    print()
    print("=" * 66)

    # 断言：正确写法必须通过；三种错误写法必须失败（证明测试有判别力，不是空转）
    bad = [n for n, ok, want in results if ok != want]
    if bad:
        print(f" 结果：不通过 —— 以下用例判定与预期不符：{bad}")
        return 1
    print(" 结果：通过 —— 只有「不平移不翻转」成立，三种翻转写法都能被识别为错误")
    print("   结论：贴图调用必须是 nvgImagePattern(vg, 0, 0, W, H, 0, img, 1)、flags = 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
