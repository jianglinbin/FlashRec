#!/usr/bin/env python3
"""由 assets/skins.json 生成 design/ui_skins_preview.html 的 SKINS 数据块（消除第四处真值）。

保留 HTML 的 CSS / mockup 结构不动，只重写 `const SKINS = [ ... ];`。
用法：python tools/gen_skins_preview.py
"""
import json, os, re, io, sys
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKINS = os.path.join(ROOT, "assets", "skins.json")
HTML = os.path.join(ROOT, "design", "ui_skins_preview.html")

doc = json.load(open(SKINS, encoding="utf-8"))
layout = doc["layout"]
lay_defaults = {
    "knobSize": layout["progress"]["knobSize"],
    "playSize": layout["playButtonCenter"]["size"],
}

# JSON 颜色名 -> HTML 字段名（与 ui_skins_preview.html 的 mockup 用法对应）
CMAP = [("bar", "barBg"), ("border", "barBorderTopBar"), ("title", "titleText"),
        ("sub", "subText"), ("badgeBg", "badgeBg"), ("badgeFg", "badgeText"),
        ("icon", "winIcon"), ("track", "track"), ("buffer", "buffer"),
        ("played", "played"), ("knob", "knob"), ("tick", "chapterTick"),
        ("playBg", "playButtonBg"), ("playBorder", "playButtonBorder"),
        ("playIcon", "playButtonIcon")]


def js_skin(s):
    c = s["colors"]
    sh = s.get("shape", {})
    knob_w = sh.get("knobSizeOverride", lay_defaults["knobSize"])
    play_w = sh.get("playButtonSizeOverride", lay_defaults["playSize"])
    knob_r = "50%" if sh.get("knobShape", "circle") == "circle" else "0"
    play_r = "50%" if sh.get("playButtonShape", "circle") == "circle" else "0"
    parts = ['  { id:"%s", name:"%s", personality:"%s", risk:"%s",' %
             (s["id"], s.get("name", s["id"]), s.get("personality", ""), s.get("risk", ""))]
    parts.append('    winR:%g, pr:%g, knobW:%g, knobR:"%s", playW:%g, playR:"%s",' %
                 (sh.get("windowRadius", 12), sh.get("progressRadius", 0), knob_w, knob_r,
                  play_w, play_r))
    parts.append('    ' + ", ".join('%s:"%s"' % (hk, c.get(jk, "")) for hk, jk in CMAP) + ',')
    sw = [("条底", "barBg"), ("强调", "played"), ("缓冲", "buffer"), ("轨道", "track")]
    parts.append('    sw:[' + ", ".join('["%s","%s"]' % (k, c.get(v, "")) for k, v in sw) + '] }')
    return "\n".join(parts)


block = "const SKINS = [\n" + ",\n\n".join(js_skin(s) for s in doc["skins"]) + "\n];"
html = open(HTML, encoding="utf-8").read()
new = re.sub(r"const SKINS = \[.*?\n\];", block, html, count=1, flags=re.S)
if new == html:
    print("未匹配到 SKINS 块，未改动"); sys.exit(1)
open(HTML, "w", encoding="utf-8", newline="\n").write(new)
print("regenerated", HTML, "(" , len(doc["skins"]), "skins )")
