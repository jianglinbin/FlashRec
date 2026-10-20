#!/usr/bin/env python3
"""通用版本台账流水号校验（project-doc-rules 提供）。

检查项：
  1. 编号 dN 全局连续（d1..dN），不跳号、不重号
  2. 单本台账内部编号连续
  3. INDEX.md 中每个版本的编号区间与条数，与实际条目一致
  4. 条目为管道表行「| dN | 日期 | 类型 | 议题 | 决议 | 依据 | 改动 | 关联 |」
     （固定 8 列，格式即契约；单元格内出现竖线 → 列数 != 8 → 判损坏；
       验证不设列——收尾校验是机器强制门槛，自定义验证以（验证：…）并入决议列）
  5. 已归档编号区间豁免：ledger/archives.json（可选）登记的区间在主线
     ledger/ 缺席属预期，不判损坏、只提示；与主线已有条目重叠时提示核对
  6. 软提示（不判损坏）：议题单元格 >100 字符（应为一句话问题）、
     决议单元格 >200 字符（应为一句话结论，细节移规格/规则文档）、
     INDEX 状态描述 >100 字符（应为一句话版本主题）

archives.json 格式（无归档历史可不建该文件）：
    [{"lo": 10, "hi": 20, "branch": "archive/some-line"}]

用法：
    python tools/ledger_check.py       # 退出码 0 = 通过，1 = 台账损坏
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

PROJECT_NAME = "FlashRec"  # 项目化：改成你的项目名（仅影响输出标题）

ROOT = Path(__file__).resolve().parent.parent
LEDGER_DIR = ROOT / "ledger"
INDEX = LEDGER_DIR / "INDEX.md"
ARCHIVES_JSON = LEDGER_DIR / "archives.json"
SKIP = {"LEDGER_RULES.md", "INDEX.md"}

ENTRY_RE = re.compile(r"^\|\s*d(\d+)\s*\|")
VER_RE = re.compile(r"^v[\w.\-]+$")
RANGE_RE = re.compile(r"^d(\d+)\s*[–\-~至]\s*d(\d+)$")


def load_archived_ranges() -> list[tuple[int, int, str]]:
    """读 ledger/archives.json -> [(起始编号, 结束编号, 归档分支)]"""
    if not ARCHIVES_JSON.is_file():
        return []
    try:
        data = json.loads(ARCHIVES_JSON.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print(f"[FAIL] archives.json 无法解析：{exc}")
        sys.exit(1)
    out = []
    for item in data:
        out.append((int(item["lo"]), int(item["hi"]), str(item.get("branch", "?"))))
    return out


def parse_index() -> tuple[list[tuple[str, str, int, int, int | None, str]], list[str]]:
    """解析 INDEX.md 表格 -> [(版本, 文件名, 起始号, 结束号, 条数, 状态)]"""
    rows: list[tuple[str, str, int, int, int | None, str]] = []
    problems: list[str] = []
    if not INDEX.is_file():
        return rows, ["找不到 ledger/INDEX.md"]
    for line in INDEX.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) < 6:
            continue
        ver, fname, rng, _dates, cnt, status = cells[:6]
        if not VER_RE.match(ver):
            continue
        fn = fname.strip("`").strip()
        m = RANGE_RE.match(rng)
        if not m:
            problems.append(f"INDEX.md：{ver} 的编号区间 '{rng}' 无法解析（应为 dA – dB）")
            continue
        try:
            count = int(cnt)
        except ValueError:
            count = None
            problems.append(f"INDEX.md：{ver} 的条数 '{cnt}' 不是数字")
        rows.append((ver, fn, int(m.group(1)), int(m.group(2)), count, status))
    if not rows:
        problems.append("INDEX.md：没有解析到任何版本行")
    return rows, problems


def main() -> int:
    problems: list[str] = []
    notes: list[str] = []
    archived = load_archived_ranges()

    if not LEDGER_DIR.is_dir():
        print("[FAIL] 找不到 ledger/ 目录")
        return 1

    files = sorted(p for p in LEDGER_DIR.glob("*.md") if p.name not in SKIP)
    if not files:
        print("[FAIL] ledger/ 下没有台账文件")
        return 1

    per_file: dict[str, list[int]] = {}
    owners: dict[int, list[str]] = {}

    verbose_rows: list[str] = []
    for f in files:
        nums: list[int] = []
        lines = f.read_text(encoding="utf-8").splitlines()
        for line in lines:
            m = ENTRY_RE.match(line.strip())
            if not m:
                continue
            n = int(m.group(1))
            nums.append(n)
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if len(cells) != 8:
                problems.append(
                    f"{f.name}：d{n} 行为 {len(cells)} 列（应为 8 列："
                    "编号/日期/类型/议题/决议/依据/改动/关联；单元格内禁用竖线）"
                )
            else:
                if len(cells[3]) > 100:  # 软提示：议题应为一句话问题
                    verbose_rows.append(f"{f.name} d{n} 议题 {len(cells[3])} 字符")
                if len(cells[4]) > 200:  # 软提示：决议应为一句话结论
                    verbose_rows.append(f"{f.name} d{n} 决议 {len(cells[4])} 字符")
        per_file[f.name] = nums
        if not nums:
            problems.append(
                f"{f.name}：未解析到条目（条目行须为 "
                "'| dN | 日期 | 类型 | 议题 | 决议 | 依据 | 改动 | 关联 |'）"
            )
        for a, b in zip(nums, nums[1:]):
            if b != a + 1:
                problems.append(f"{f.name}：本台账内编号不连续，d{a} 之后直接跳到 d{b}")
        for n in nums:
            owners.setdefault(n, []).append(f.name)

    for n, fs in sorted(owners.items()):
        if len(fs) > 1:
            problems.append(f"d{n} 重复出现于 {', '.join(fs)}（编号全局唯一，禁止复用）")

    nums = sorted(owners)
    if nums:
        missing = [n for n in range(1, nums[-1] + 1) if n not in owners]
        true_missing = [
            n for n in missing
            if not any(lo <= n <= hi for lo, hi, _ in archived)
        ]
        for lo, hi, br in archived:
            in_range = [n for n in missing if lo <= n <= hi]
            if in_range:
                notes.append(
                    f"d{min(in_range)}–d{max(in_range)} 属已归档区间（分支 {br}），豁免"
                )
            overlap = sorted(n for n in owners if lo <= n <= hi)
            if overlap:
                notes.append(
                    f"注意：归档区间 d{lo}–d{hi}（分支 {br}）与主线条目重叠"
                    f"（d{overlap[0]} 等），请核对 archives.json"
                )
        if true_missing:
            problems.append("断号：缺失 " + ", ".join(f"d{n}" for n in true_missing))
        else:
            notes.append(
                f"主线编号连续（含归档豁免）至 d{nums[-1]}，主线持有 {len(nums)} 条"
            )
            notes.append(f"下一个可用编号：d{nums[-1] + 1}")

    rows, idx_problems = parse_index()
    problems.extend(idx_problems)
    long_status: list[str] = []
    for ver, fn, lo, hi, count, status in rows:
        if len(status) > 100:  # 软提示：INDEX 状态描述应为一句话版本主题
            long_status.append(f"{ver}（{len(status)} 字符）")
        if fn not in per_file:
            problems.append(f"INDEX.md：{ver} 指向的 '{fn}' 不存在于 ledger/")
            continue
        actual = per_file[fn]
        if not actual:
            continue
        amin, amax = min(actual), max(actual)
        if (lo, hi) != (amin, amax):
            problems.append(
                f"INDEX.md：{ver} 区间写的是 d{lo} – d{hi}，"
                f"实际是 d{amin} – d{amax}（{fn}）"
            )
        if count is not None and count != len(actual):
            problems.append(
                f"INDEX.md：{ver} 条数写的是 {count}，实际 {len(actual)}（{fn}）"
            )

    print("=" * 56)
    print(f" {PROJECT_NAME} 台账校验")
    print("=" * 56)
    if verbose_rows:
        detail = "；".join(verbose_rows[:5]) + (f" 等 {len(verbose_rows)} 条" if len(verbose_rows) > 5 else "")
        notes.append(f"决议单元格超长（>200 字符，应为一句话结论）：{detail}——细节应移规格/规则文档")
    if long_status:
        notes.append(
            f"INDEX 状态描述超长（应为一句话主题）：{len(long_status)} 行——"
            + "；".join(long_status[:4]) + (" 等" if len(long_status) > 4 else "")
        )
    for n in notes:
        print(f"  [OK]   {n}")
    if problems:
        for p in problems:
            print(f"  [FAIL] {p}")
        print("-" * 56)
        print(f" 结果：台账损坏，{len(problems)} 处问题待修复")
        return 1
    print("-" * 56)
    print(" 结果：通过 —— 编号连续、无重号、与 INDEX 一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
