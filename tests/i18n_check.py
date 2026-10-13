#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""i18n 一致性检查（v0.6.2 d362；UI_I18N_PLAN.md §4 M3/M4）。

三道闸：
  1. 键位对齐 —— src/app/i18n.cpp 语言表行与 i18n.h 的 StrId 枚举逐一对应
     （行序错位不报编译错但显示错串，靠本检查结构化拦截）；
  2. 防回潮 —— UI 层（src/ui/**、src/app/render_loop.cpp、src/main.cpp）
     不允许出现含中文的字符串字面量（界面文案必须走 i18n::tr/trf）；
     日志行（FR_LOG_*）与注释豁免 —— 日志不是界面文案；
  3. README 双语对齐 —— README.md 与 README.en.md 二级标题数量一致（防单向漂移）。

用法：python tools/i18n_check.py （仓库根执行；进 CI/检查链）
退出码：0 = 通过，1 = 有问题。
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

FAILS: list[str] = []


def fail(msg: str) -> None:
    FAILS.append(msg)


# ---------- 1. 键位对齐 ----------

hdr = (ROOT / "src/app/i18n.h").read_text(encoding="utf-8")
m = re.search(r"enum class StrId : int \{(.*?)\};", hdr, re.S)
if not m:
    fail("i18n.h: 找不到 StrId 枚举")
    enum_names = []
else:
    body = m.group(1)
    # 去行注释，取行首标识符（枚举成员）；跳过 Count 哨兵
    names = []
    for raw in body.splitlines():
        line = raw.split("//")[0].strip().rstrip(",")
        if not line or line.startswith("/*") or line.startswith("*"):
            continue
        for tok in line.split(","):
            tok = tok.strip()
            if re.fullmatch(r"[A-Za-z_]\w*", tok):
                names.append(tok)
    enum_names = [n for n in names if n != "Count"]
    if "Count" not in names:
        fail("i18n.h: StrId 枚举缺 Count 哨兵")

src = (ROOT / "src/app/i18n.cpp").read_text(encoding="utf-8")
rows = re.findall(r"/\*\s*([A-Za-z_]\w*)\s*\*/", src)
# 只取表区（第一处 /* OsdPlay */ 之后）；文件里注释 tag 唯一
if rows and rows[0] != "OsdPlay":
    fail(f"i18n.cpp: 表区首个 tag 异常（{rows[0]}）")
if rows != enum_names:
    miss = [n for n in enum_names if n not in rows]
    extra = [n for n in rows if n not in enum_names]
    if miss:
        fail(f"i18n.cpp: 表缺行 {miss}")
    if extra:
        fail(f"i18n.cpp: 表多行 {extra}")
    if not miss and not extra and rows != enum_names:
        fail("i18n.cpp: 表行序与 StrId 枚举声明序不一致（错位 = 显示错串）")

# 每行恰好两列（zh, en）；行内字符串数按 /*tag*/ 分段校验
tbl = src[src.find("const char* const kTable"):] if "kTable" in src else ""
if tbl:
    segments = re.split(r"/\*\s*[A-Za-z_]\w*\s*\*/", tbl)[1:]
    for i, seg in enumerate(segments):
        # 取到该行结束（分号/换行的下一条 /* 前）；跨行值（如 EmptyHint）同段内
        strs = re.findall(r'"(?:[^"\\]|\\.)*"', seg.split("};")[0])
        if len(strs) != 2:
            fail(f"i18n.cpp: 表第 {i+1} 行（{rows[i] if i < len(rows) else '?'}）"
                 f"应有 2 列，实得 {len(strs)}")
        elif strs[0] == strs[1] and rows[i] not in ("LangZhName", "LangEnName"):
            # 中英同文 = 疑似偷懒（语言名本名是唯一合法例外）
            fail(f"i18n.cpp: 表行 {rows[i]} 中英同文（非语言名，疑似未翻译）")

# ---------- 2. 防回潮：UI 层中文字面量 ----------

CJK = re.compile(r"[\u4e00-\u9fff]")
STR = re.compile(r'"(?:[^"\\\n]|\\.)*"')
scan_targets = sorted((ROOT / "src/ui").rglob("*.cpp")) + \
    sorted((ROOT / "src/ui").rglob("*.h")) + \
    [ROOT / "src/app/render_loop.cpp", ROOT / "src/main.cpp"]
for f in scan_targets:
    lines = f.read_text(encoding="utf-8").splitlines()
    for ln, raw in enumerate(lines, 1):
        code = raw.split("//")[0]  # 掐掉行尾注释（防注释示例误报）
        # 日志豁免：FR_LOG 行本身 + 跨行参数（上一/上两行是调用开头或悬置续行——
        # 续行特征 = 以逗号 / 开括号 / 未闭合字符串结尾）
        def _log_ctx(i: int) -> bool:
            if i < 1:
                return False
            prev = lines[i - 1].split("//")[0].rstrip()
            return "FR_LOG_" in prev or prev.endswith((",", "(", '"'))
        if "FR_LOG_" in code or _log_ctx(ln - 1) or _log_ctx(ln - 2):
            continue  # 日志不是界面文案
        for s in STR.findall(code):
            if CJK.search(s):
                fail(f"{f.relative_to(ROOT)}:{ln}: UI 层中文字面量 {s[:40]}（应走 i18n）")

# ---------- 3. README 双语对齐 ----------

def h2_count(p: Path) -> int:
    return sum(1 for l in p.read_text(encoding="utf-8").splitlines()
               if l.startswith("## "))

zh, en = ROOT / "README.md", ROOT / "README.en.md"
if not en.exists():
    fail("README.en.md 不存在（双语 README 未落地）")
else:
    c2, c1 = h2_count(en), h2_count(zh)
    if c1 != c2:
        fail(f"README 二级标题数不一致：zh={c1} en={c2}（双语需同步维护）")
    for p, other in ((zh, "README.en.md"), (en, "README.md")):
        head = "\n".join(p.read_text(encoding="utf-8").splitlines()[:8])
        if other not in head:
            fail(f"{p.name}: 顶部缺语言切换链接（应含 {other} 链接）")

# ---------- 结果 ----------

if FAILS:
    print(f"i18n 检查：FAIL（{len(FAILS)} 项）")
    for x in FAILS:
        print("  [X]", x)
    sys.exit(1)
print(f"i18n 检查：通过 —— 键位 {len(enum_names)} 条中英对齐；"
      f"UI 层无中文字面量回潮；README 双语对齐")
