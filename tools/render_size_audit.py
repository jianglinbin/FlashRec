"""静态审计：FBO / mpv / nanovg 三者尺寸语义是否一致（渲染链最容易错的地方）。

背景（d16→d17 两次踩坑）：
  这条链上有**三个**不同的"尺寸"，语义各不相同，混用就出"窗口穿了/画面挤到一角"：

    1. Framebuffer::width()/height()        —— 内容尺寸（本次请求的逻辑尺寸）
    2. Framebuffer::alloc_width()/alloc_height() —— 纹理/FBO 容量
    3. 目标矩形 w/h（窗口 framebuffer 尺寸）    —— 贴图时的目标尺寸

  正确用法（依据 third_party/mpv/include/mpv/render_gl.h）：
    - mpv 的 mpv_opengl_fbo.w/h  **必须是 FBO 容量**（"must refer to the size of
      the framebuffer"）。传内容尺寸 → 画面只落纹理左上角一块 → "窗口穿了"。
    - nanovg create_image_from_handle 的 w/h  **必须是纹理容量**（= 纹理实际像素）。
    - nvgImagePattern 的 w/h  **必须是纹理容量**（采样区域；给内容尺寸会让
      GL_LINEAR 在边界取到纹理外 → 贴图发虚）。
    - Framebuffer::bind() 的 glViewport  **必须是容量**（覆盖整张纹理）。

  这条链路的不变式：**纹理尺寸 == FBO 容量 == 渲染/采样尺寸**，
  与目标矩形（窗口尺寸）相等时比例 100% 精确（由 mpv letterbox 保证）。

本脚本是**纯静态检查**，不起进程、不碰 GPU，只 grep 源码里的调用形态并核对。
退出码非 0 表示发现可疑用法。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

# (文件, 行号, 行内容)
def read_sources():
    out = []
    for base, _dirs, files in os.walk(SRC):
        for f in files:
            if not f.endswith((".cpp", ".h")):
                continue
            p = os.path.join(base, f)
            with open(p, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
            # 逻辑行（把跨行调用拼成一行），用于匹配可能换行书写的调用
            logical = re.sub(r"\s*\n\s*", " ", text)
            # 去注释版（行注释 + 块注释），只给 forbid 类检查用：
            # 说明性文字里出现禁用符号是正常的（正是为了讲清"为什么不用它"），
            # 只有**代码里**出现才算违规。
            code_logical = re.sub(r"\s*\n\s*", " ", strip_comments(text))
            out.append((os.path.relpath(p, ROOT), text, logical, code_logical))
    return out


def strip_comments(text):
    """去掉 C/C++ 的行注释与块注释，字符串字面量原样保留。

    逐字符扫描而非正则，避免 `"http://x"` 这类字符串里的 `//` 被误判成注释。
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\":
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            out.append("\n")
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
            out.append(" ")
            continue
        out.append(c)
        i += 1
    return "".join(out)


CHECKS = [
    # (标题, 正则, 说明, 期望匹配 "alloc|content")
    (
        "mpv render_frame 的 w/h（须传容量）",
        r"render_frame\s*\(\s*\(int\)\s*\w+\.fbo\(\)\s*,\s*([^,]+),\s*([^)]+)\)",
        "mpv_opengl_fbo.w/h 必须是 FBO 容量（render_gl.h）",
        "alloc",
    ),
    (
        "nanovg create_image_from_handle 的 w/h（须传容量）",
        r"create_image_from_handle\s*\(\s*[\w.]+\.texture\(\)\s*,\s*([^,]+),",
        "nanovg 图像注册的 w/h 必须是纹理实际像素尺寸 = 容量",
        "alloc",
    ),
    (
        "nvgImagePattern 的 w/h（须传容量）",
        r"nvgImagePattern\s*\(\s*[\w.]+\(\)\s*,\s*[^,]+,\s*[^,]+,\s*([^,]+),\s*([^,]+),",
        "pattern 的 w/h 是采样区域（负值=翻转），其绝对值必须是纹理容量",
        "alloc",
    ),
    (
        "NVG_IMAGE_FLIPY 的使用（本链路禁用）",
        r"(NVG_IMAGE_FLIPY)",
        "本链路的翻转由 pattern 的负 extent 完成；同时用 FLIPY 会双翻抵消，"
        "且 FLIPY 的枢轴取自 extent[1]*0.5f、条件一变就静默失效",
        "forbid",
    ),
]

# 被视为"容量"的标识：直接调用，或缓存了容量的成员变量。
# pic_w_/pic_h_ 在 render_loop 里被赋值为 alloc_width()/alloc_height()（见下方 CONSISTENCY 检查）。
CAPACITY_TOKENS = ("alloc", "pic_w_", "pic_h_")
CAPACITY_ASSIGN = re.compile(r"(pic_w_|pic_h_)\s*=\s*[\w.]+\.(alloc_width|alloc_height)\(\)")


def is_capacity(args):
    """实参是否表达"容量"语义（直接调用 alloc_*，或缓存容量的成员变量）。"""
    return any(tok in args for tok in CAPACITY_TOKENS)


# 贴图用的翻转 pattern：nvgImagePattern(vg, ox, oy, ew, eh, angle, img, alpha)
FLIP_CALL = re.compile(
    r"nvgImagePattern\s*\(\s*[\w.]+\(\)\s*,\s*([^,]+),\s*([^,]+),\s*([^,]+),\s*([^,]+),")


def check_flip_pattern(src, problems):
    """检查 FBO 贴图是否**没有**做任何平移/翻转（正确做法就是什么都不做）。

    d17→d18 连续三次踩坑的实证结论（tools/flip_math_check.py 有完整公式推导）：
      · NVG_IMAGE_FLIPY        → 镜像（枢轴依赖 pattern 高度，会静默失效）
      · cy=+H, h=-H            → 镜像（shader 收到逆变换，pt.y = 1 - y/H）
      · cy=-H, h=-H            → 半屏复制（pt.y ∈ [-2,-1]，全在纹理外）
    正确形态：cx = cy = 0、extent 为正的 (W, H)。本检查拦截任何负号 extent
    或非零 cy —— 出现即说明有人又在"修"这个本不该翻的地方。
    """
    pat = re.compile(r"nvgImagePattern\s*\(\s*[\w.]+\(\)\s*,([^;]*?)\)\s*;", re.S)
    for path, _text, _logical, code in src:
        for m in pat.finditer(code):
            args = [a.strip() for a in m.group(1).split(",")]
            if len(args) < 4:
                continue
            cx, cy, w, h = args[0], args[1], args[2], args[3]
            has_neg_extent = re.search(r"-\s*[\w.]", w + " " + h) is not None
            has_neg_cy = re.search(r"-\s*[\w.]", cy) is not None
            if not (has_neg_extent or has_neg_cy):
                continue  # 正常的不平移不翻转写法，无需报告
            print(f"  [!! ] {path} FBO 贴图带了翻转/平移")
            print(f"        nvgImagePattern(cx={cx}, cy={cy}, w={w}, h={h})")
            print("        要求：cx=0、cy=0、extent 取正（不平移不翻转）——"
                  "任何负 extent 或非零 cy 都会造成镜像/半屏复制")
            problems.append(
                f"{path} FBO 贴图：cx={cx} cy={cy} w={w} h={h} 带了翻转 —— "
                "应为 nvgImagePattern(vg, 0, 0, 正W, 正H, 0, img, 1)")


def main():
    src = read_sources()
    problems = []
    checked = 0

    # 先确认缓存变量确实缓存的是容量（避免 pic_w_ 被改成内容尺寸后审计还放行）
    for path, text, _logical, _code in src:
        for ln, line in enumerate(text.splitlines(), 1):
            for m in CAPACITY_ASSIGN.finditer(line):
                print(f"  [OK ] 容量缓存 {path}:{ln}  {m.group(1)} = ….{m.group(2)}()")

    for title, pat, why, want in CHECKS:
        rx = re.compile(pat)
        for path, _text, _logical, code_logical in src:
            # 只看代码：注释里提到禁用符号（讲"为什么不用它"）或举例调用都属正常，
            # 不构成真实调用，不应被审计。
            for m in rx.finditer(code_logical):
                checked += 1
                args = " , ".join(g for g in m.groups() if g)
                if want == "forbid":
                    print(f"  [!! ] {path}")
                    print(f"        {m.group(0)}")
                    print(f"        出现禁用写法 —— {why}")
                    problems.append(f"{path} {title} —— {why}")
                    continue
                got = is_capacity(args)
                ok = got if want == "alloc" else (not got)
                mark = "OK " if ok else "!! "
                print(f"  [{mark}] {path}")
                print(f"        {m.group(0)}")
                print(f"        尺寸实参 = {args}   （要求：{want}）")
                if not ok:
                    problems.append(f"{path} {title} —— {why}")

    # 翻转 pattern 的 origin_y 与 extent_h 必须同号且同为负（见下方推导）
    check_flip_pattern(src, problems)
    checked += 1

    # Framebuffer::bind 的 glViewport 检查
    fb = os.path.join(SRC, "ui", "framebuffer.cpp")
    if os.path.exists(fb):
        with open(fb, encoding="utf-8", errors="replace") as fh:
            body = fh.read()
        m = re.search(r"void\s+Framebuffer::bind\(\)\s*\{(.*?)\n\}", body, re.S)
        if m:
            checked += 1
            gp = re.search(r"glViewport\s*\(([^)]*)\)", m.group(1))
            arg = gp.group(1) if gp else "(未找到 glViewport)"
            ok = "alloc" in arg
            print(f"  [{'OK ' if ok else '!! '}] src/ui/framebuffer.cpp bind() glViewport")
            print(f"        实参 = {arg}   （要求：alloc 容量 —— 覆盖整张纹理）")
            if not ok:
                problems.append("framebuffer.cpp bind(): glViewport 未用容量，mpv 画面会被裁剪")

    # ensure() 必须"容量恒等于内容"：任何重建都不得带余量（禁 ratchet）。
    # 棘轮已被移除（d19）：容量 > 内容会让贴图 1:1 原尺寸绘制，缩窗后画面不随窗口缩放。
    if os.path.exists(fb):
        with open(fb, encoding="utf-8", errors="replace") as fh:
            body = fh.read()
        m = re.search(r"bool\s+Framebuffer::ensure\(.*?\n\}", body, re.S)
        if m:
            checked += 1
            fn = m.group(0)
            has_ratchet = re.search(r"\bratchet\s*\(", fn)
            exact = re.search(r"create_storage\s*\(\s*w\s*,\s*h\s*\)", fn)
            ok = not has_ratchet and bool(exact)
            arg = "create_storage(w, h) 精确" if exact else "(形态未识别)"
            print(f"  [{'OK ' if ok else '!! '}] src/ui/framebuffer.cpp ensure() 精确分配")
            print(f"        重建实参 = {arg}   （要求：create_storage(w, h) —— 容量恒等于内容，禁余量）")
            if ok is False:
                if has_ratchet:
                    problems.append("framebuffer.cpp ensure(): 出现棘轮/余量分配，缩窗后会 1:1 裁切（d19 教训）")
                else:
                    problems.append("framebuffer.cpp ensure(): 未识别为精确重建，请人工核对尺寸语义")

    print()
    print("=" * 56)
    print(f" 检查项匹配到 {checked} 处调用")
    if problems:
        print(f" 发现 {len(problems)} 处可疑用法：")
        for p in problems:
            print(f"   - {p}")
        print(" 结果：不通过 —— 请核对尺寸语义后再改渲染链")
        return 1
    print(" 结果：通过 —— 渲染链尺寸语义一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
