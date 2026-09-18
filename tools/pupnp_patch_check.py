"""校验 pupnp 构建期锚点补丁的锚点/替换文本能否在源码里精确命中。

为什么需要它：补丁机制本身只在 **configure 期**才判定锚点命中（未命中直接
FATAL_ERROR）。等到那时候才发现锚点写错，代价是「改了 cmake → 跑一次完整
configure → 报错 → 再改」的往返。这个脚本在**不开构建系统**的前提下，把
cmake/*.cmake 里的锚点与替换文本原样解析出来，逐个对 third_party 源码做
字节级命中检查，几秒钟就能定位问题。

它同时回答一个问题：**当前源码树处于「已打补丁」还是「原始」状态** ——
每个锚点只应命中其中一个（两个都命中或都不命中都是异常）。

解析规则（与 CMake 语义保持一致）：
  · 中括号参数 [[...]]  —— 内容原样，且 CMake 会把 CRLF 归一为 LF
  · 双引号参数 "..."    —— 支持 \\t \\n \\r \\\\ \\" 转义
  · 裸参数              —— 到空白/换行/右括号为止

用法：
    python tools/pupnp_patch_check.py            # 全部模块
    python tools/pupnp_patch_check.py -v         # 附带每次命中的行号
"""

import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PUPNP = os.path.join(ROOT, 'third_party', 'pupnp')
CMAKE_DIR = os.path.join(ROOT, 'cmake')

# 补丁模块 -> 参数个数。两处机制：泛型（开关变量作首参）与多网卡包装（3 参）。
MODULES = {
    'pupnp_multi_if.cmake': ('fr_pupnp_toggle', 3),
    'pupnp_http_trace.cmake': ('fr_pupnp_patch_toggle', 4),
}

ESCAPES = {'t': '\t', 'n': '\n', 'r': '\r', '\\': '\\', '"': '"', ' ': ' '}


def parse_bracket(text, i):
    """i 指向 '[[ ' 的第二个字符之后。返回 (内容, 下一个位置)。"""
    end = text.index(']]', i)
    raw = text[i + 2:end]
    return raw.replace('\r\n', '\n'), end + 2


def parse_quoted(text, i):
    """i 指向开引号之后。"""
    out = []
    n = len(text)
    while i < n:
        c = text[i]
        if c == '\\' and i + 1 < n:
            nxt = text[i + 1]
            out.append(ESCAPES.get(nxt, '\\' + nxt))
            i += 2
            continue
        if c == '"':
            return ''.join(out), i + 1
        out.append(c)
        i += 1
    raise ValueError('unterminated quoted argument')


def parse_call_args(text, start):
    """从 start（左括号之后）解析实参列表。返回 (args, 右括号位置)。"""
    args = []
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        if c in ' \t\r\n':
            i += 1
            continue
        if c == ')':
            return args, i
        if text.startswith('[[', i):
            val, i = parse_bracket(text, i)
            args.append(val)
            continue
        if c == '"':
            val, i = parse_quoted(text, i + 1)
            args.append(val)
            continue
        if c == '#':
            while i < n and text[i] != '\n':
                i += 1
            continue
        j = i
        while j < n and text[j] not in ' \t\r\n)':
            j += 1
        args.append(text[i:j])
        i = j
    raise ValueError('unterminated argument list')


def collect_calls(path, fname, nargs):
    text = io.open(path, 'r', encoding='utf-8', newline='').read()
    calls = []
    for m in re.finditer(r'(?m)^\s*' + re.escape(fname) + r'\(', text):
        paren = text.index('(', m.end() - 1)
        try:
            args, _ = parse_call_args(text, paren + 1)
        except ValueError as exc:  # 解析不动就跳过，别把工具本身变成阻塞点
            print('  [!] %s: %s' % (os.path.basename(path), exc))
            continue
        if len(args) != nargs or any('${' in a for a in args):
            # 变量实参（非字面量）不在此脚本职责内
            continue
        calls.append(args)
    return calls


def normalize(s):
    return s.replace('\r\n', '\n')


def main():
    verbose = '-v' in sys.argv
    rows = []
    states = []
    problems = []

    for mod, (fname, nargs) in MODULES.items():
        path = os.path.join(CMAKE_DIR, mod)
        if not os.path.exists(path):
            print('  [!] 缺少模块 %s' % mod)
            continue
        for args in collect_calls(path, fname, nargs):
            if nargs == 4:
                enable, rel, anchor, replacement = args
            else:
                enable, (rel, anchor, replacement) = 'FR_MULTI_IF', args
            target = os.path.join(PUPNP, rel.replace('/', os.sep))
            if not os.path.exists(target):
                problems.append('%s: 找不到目标文件 %s' % (mod, rel))
                continue
            src = normalize(io.open(target, 'r', encoding='utf-8', newline='').read())
            a_hits = src.count(anchor)
            r_hits = src.count(replacement)
            rows.append((mod, rel, enable, a_hits, r_hits, anchor, replacement))

    print('=' * 78)
    print(' pupnp 锚点补丁校验')
    print('=' * 78)
    print('  %-22s %-32s %-5s %-6s %s' % ('模块', '目标文件', '锚点', '替换后', '判定'))
    fatal = 0
    for mod, rel, enable, a, r, anchor, replacement in rows:
        short = rel.split('/')[-1]
        sub = anchor in replacement  # 锚点是替换文本的**子串**（前缀/后缀/中缀）
        note = ''
        if (a, r) == (0, 0):
            state = '锚点丢失'
            note = '  <== 源码结构已变，configure 会 FATAL_ERROR'
            fatal += 1
        elif (a, r) == (0, 1):
            state = '已打'
        elif (a, r) == (1, 0):
            state = '原始'
        elif sub:
            # 锚点本身是替换文本的子串（如 A0/A2/S1/S4/W1 的替换以锚点开头）：
            # 「两个都在」= 已打，这是正常状态，不是异常。
            state = '已打'
        else:
            state = '歧义'
            note = '  <== 锚点与替换文本同时存在且互不包含：OFF 还原会留下重复代码'
            fatal += 1
        states.append(state)
        print('  %-22s %-32s %-5d %-6d %s%s' % (mod, short, a, r, state, note))

    # 同一模块的锚点必须整体同态（模块级开关，不存在只打一半的合法状态）。
    # 注意：新增锚点之后、下一次 configure 之前，新老锚点状态必然不一致 ——
    # 那是过程状态，不是错误，这里只提示"跑一次 configure 即可统一"。
    warn = []
    by_mod = {}
    for mod, state in zip([x[0] for x in rows], states):
        if state in ('锚点丢失', '歧义'):
            continue
        by_mod.setdefault(mod, []).append(state)
    for mod, mod_states in by_mod.items():
        if len(set(mod_states)) > 1:
            warn.append('%s：%d 个锚点里 %d 个已打 / %d 个原始 —— 状态不一致'
                        % (mod, len(mod_states),
                           mod_states.count('已打'), mod_states.count('原始')))

    if verbose:
        print('-' * 78)
        for mod, rel, enable, a, r, anchor, replacement in rows:
            print('  %s :: %s' % (rel, anchor.split('\n')[0][:58]))

    print('-' * 78)
    print('  共 %d 个锚点：已打 %d / 原始 %d / 致命 %d'
          % (len(states), states.count('已打'), states.count('原始'), fatal))
    for w in warn:
        print('  [注意] ' + w)
    if warn:
        print('         （新增/删除锚点后、下一次 configure 之前属正常现象；'
              '跑一次 configure 会统一到开关值）')
    if fatal:
        print(' 结果：失败 —— 有问题锚点，configure 会 FATAL_ERROR 或产生重复代码')
        return 1
    print(' 结果：通过 —— 所有锚点/替换文本都能精确命中，无歧义')
    return 0


if __name__ == '__main__':
    sys.exit(main())
