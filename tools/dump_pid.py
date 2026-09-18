#!/usr/bin/env python3
"""对指定 PID 抓取完整 minidump（MiniDumpWithFullMemory，只读不改）。

用法: python tools/dump_pid.py <pid> [out.dmp]
默认输出 %TEMP%/flashrec_diag/<pid>.dmp
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys

PROCESS_ALL_ACCESS = 0x1F0FFF
MiniDumpWithFullMemory = 0x2
MiniDumpWithHandleData = 0x4
MiniDumpWithUnloadedModules = 0x20
MiniDumpWithProcessThreadData = 0x100


def main():
    pid = int(sys.argv[1])
    out = (sys.argv[2] if len(sys.argv) > 2
           else os.path.join(os.environ["TEMP"], "flashrec_diag", f"{pid}.dmp"))
    os.makedirs(os.path.dirname(out), exist_ok=True)

    dbg = ctypes.windll.dbghelp
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not h:
        print(f"OpenProcess({pid}) failed err={k32.GetLastError()}")
        return 1
    try:
        fs = k32.CreateFileW(out, 0x40000000, 0, None, 2, 0, None)  # GENERIC_WRITE, CREATE_ALWAYS
        if fs == -1 or fs == 0xFFFFFFFFFFFFFFFF:
            print(f"CreateFileW failed err={k32.GetLastError()}")
            return 1
        ok = dbg.MiniDumpWriteDump(h, pid, fs, MiniDumpWithFullMemory, None, None, None)
        err = k32.GetLastError()
        k32.CloseHandle(fs)
        size = os.path.getsize(out) if os.path.exists(out) else 0
        print(f"{'OK' if ok else 'FAILED'} err={err} size={size/1e6:.1f}MB -> {out}")
        return 0 if ok else 1
    finally:
        k32.CloseHandle(h)


if __name__ == "__main__":
    sys.exit(main())
