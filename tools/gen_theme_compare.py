# 生成 src/ui/theme_compare.h：Theme 逐字段比较（避免 memcmp 的 padding 陷阱）。
# 真值 = src/ui/theme.h；theme.h 增删字段后必须重跑本脚本，否则皮肤自检（FR_SKIN_SELFCHECK=1）
# 会漏比新字段。
# 运行：python tools/gen_theme_compare.py
import re

SRC = r'D:\dev\FlashRec\src\ui\theme.h'
OUT = r'D:\dev\FlashRec\src\ui\theme_compare.h'

d = open(SRC, encoding='utf-8').read()
# 颜色成员：允许带默认初始化（如 `NVGcolor stageBorder = {0, 0, 0, 0};`）；
# 函数声明 `NVGcolor parse_color(const char*);` 因括号在名后而不匹配。
cols = re.findall(r'NVGcolor\s+(\w+)\s*(?:=\s*\{[^}]*\})?\s*;', d)
i = d.find('struct Shape')
j = d.find('} shape;')
shape = re.findall(r'(float|bool)\s+(\w+)\s*=', d[i:j])
i2 = d.find('struct Layout')
j2 = d.find('} layout;')
lay = re.findall(r'(float|bool|double|int)\s+(\w+)\s*=', d[i2:j2])

L = []
L.append('#pragma once')
L.append('// 自动生成：Theme 逐字段比较（避免 memcmp 的 padding 陷阱）。由 tools/gen_theme_compare.py 生成，勿手改。')
L.append('#include "ui/theme.h"')
L.append('')
L.append('namespace fr {')
L.append('inline const char* theme_first_diff(const Theme& a, const Theme& b) {')
L.append('  auto C = [](NVGcolor x, NVGcolor y) { return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a; };')
for n in cols:
    L.append('  if (!C(a.%s, b.%s)) return "%s";' % (n, n, n))
for ty, n in shape:
    L.append('  if (a.shape.%s != b.shape.%s) return "shape.%s";' % (n, n, n))
for ty, n in lay:
    L.append('  if (a.layout.%s != b.layout.%s) return "layout.%s";' % (n, n, n))
L.append('  return nullptr;')
L.append('}')
L.append('}  // namespace fr')
open(OUT, 'w', encoding='utf-8', newline='\n').write('\n'.join(L) + '\n')
print('written theme_compare.h lines', len(L), '| colors', len(cols), 'shape', len(shape), 'layout', len(lay))
