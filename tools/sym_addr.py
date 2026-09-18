#!/usr/bin/env python3
"""用 flashrec PDB 解析栈扫描出的返回地址 → 函数名。

用法: python tools/sym_addr.py <exe_or_pdb_dir> <base_hex> <rva...>
示例: python tools/sym_addr.py build/bin 140000000 BBE0B B1A66 AFC51 7CAE9 30DB8
"""
import ctypes
import sys
from ctypes import wintypes

SYMOPT_UNDNAME = 0x2
SYMOPT_DEFERRED_LOADS = 0x4


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wintypes.ULONG),
                ("TypeIndex", wintypes.ULONG),
                ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wintypes.ULONG),
                ("Size", wintypes.ULONG),
                ("ModBase", ctypes.c_uint64),
                ("Flags", wintypes.ULONG),
                ("Value", ctypes.c_uint64),
                ("Address", ctypes.c_uint64),
                ("Register", wintypes.ULONG),
                ("Scope", wintypes.ULONG),
                ("Tag", wintypes.ULONG),
                ("NameLen", wintypes.ULONG),
                ("MaxNameLen", wintypes.ULONG),
                ("Name", ctypes.c_wchar * 256)]


def main():
    import os
    search = os.path.abspath(sys.argv[1])
    base = int(sys.argv[2], 16)
    dbghelp = ctypes.windll.dbghelp
    dbghelp.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS)
    hp = ctypes.windll.kernel32.GetCurrentProcess()
    if not dbghelp.SymInitializeW(hp, ctypes.c_wchar_p(search), False):
        print(f"SymInitialize failed err={ctypes.windll.kernel32.GetLastError()}")
        return 1
    # 模块名随意，加载 exe（PDB 同目录同名自动找到）
    exe = search.rstrip("/\\") + "\\flashrec.exe"
    if not dbghelp.SymLoadModuleExW(hp, None, ctypes.c_wchar_p(exe), None,
                                    ctypes.c_uint64(base), 0, None, 0):
        print(f"SymLoadModuleExW failed err={ctypes.windll.kernel32.GetLastError()}")
        return 1
    for a in sys.argv[3:]:
        rva = int(a, 16)
        addr = base + rva
        si = SYMBOL_INFOW()
        si.SizeOfStruct = 88
        si.MaxNameLen = 256
        disp = ctypes.c_uint64(0)
        ok = dbghelp.SymFromAddrW(hp, ctypes.c_uint64(addr), ctypes.byref(disp), ctypes.byref(si))
        if ok:
            print(f"+0x{rva:X}  {si.Name}+0x{disp.value:X}  (size={si.Size})")
        else:
            print(f"+0x{rva:X}  ??? err={ctypes.windll.kernel32.GetLastError()}")


if __name__ == "__main__":
    sys.exit(main())
