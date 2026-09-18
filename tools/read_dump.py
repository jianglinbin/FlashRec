#!/usr/bin/env python3
"""解析 minidump：线程表 + RIP 模块映射 + 栈内返回地址扫描（x64 无帧指针启发式）。

用法: python tools/read_dump.py <dump.dmp> [--scan-tid TID]
"""
import struct
import sys

# stream types
ThreadListStream, ModuleListStream, ExceptionStream, Memory64ListStream = 3, 4, 6, 9


class Dump:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        sig, ver, n, dir_rva = struct.unpack_from("<IIII", self.d, 0)
        assert sig == 0x504D444D, "not MDMP"
        self.mods = []  # (base, size, name)
        self.threads = []  # dict per thread
        self.mem = []  # (start, size, file_off) from Memory64ListStream
        for i in range(n):
            t, dsz, rva = struct.unpack_from("<III", self.d, dir_rva + 12 * i)
            if t == ModuleListStream:
                self._modules(rva)
            elif t == ThreadListStream:
                self._threads(rva)
            elif t == Memory64ListStream:
                self._mem64(rva)

    def _mem64(self, rva):
        n, base_rva = struct.unpack_from("<QQ", self.d, rva)
        off = base_rva
        for i in range(n):
            start, size = struct.unpack_from("<QQ", self.d, rva + 16 + 16 * i)
            self.mem.append((start, size, off))
            off += size

    def read_va(self, addr, size):
        """Read virtual memory via Memory64ListStream (full-memory dumps)."""
        out = bytearray()
        while size > 0:
            hit = None
            for start, sz, foff in self.mem:
                if start <= addr < start + sz:
                    hit = (start, sz, foff)
                    break
            if not hit:
                out += b"\x00" * size
                return bytes(out)
            start, sz, foff = hit
            take = min(sz - (addr - start), size)
            out += self.d[foff + (addr - start): foff + (addr - start) + take]
            addr += take
            size -= take
        return bytes(out)

    def _modules(self, rva):
        (n,) = struct.unpack_from("<I", self.d, rva)
        for i in range(n):
            b = rva + 4 + 108 * i
            base, size = struct.unpack_from("<QQ", self.d, b, )[0], struct.unpack_from("<I", self.d, b + 8)[0]
            name_rva = struct.unpack_from("<I", self.d, b + 20)[0]
            (slen,) = struct.unpack_from("<I", self.d, name_rva)
            name = self.d[name_rva + 4: name_rva + 4 + slen].decode("utf-16le", "replace")
            self.mods.append((base, size, name))

    def _threads(self, rva):
        (n,) = struct.unpack_from("<I", self.d, rva)
        for i in range(n):
            b = rva + 4 + 48 * i
            tid = struct.unpack_from("<I", self.d, b)[0]
            st_start = struct.unpack_from("<Q", self.d, b + 24)[0]
            st_size = struct.unpack_from("<I", self.d, b + 32)[0]
            st_rva = struct.unpack_from("<I", self.d, b + 36)[0]
            ctx_rva = struct.unpack_from("<I", self.d, b + 44)[0]
            rip = struct.unpack_from("<Q", self.d, ctx_rva + 0xF8)[0]
            rsp = struct.unpack_from("<Q", self.d, ctx_rva + 0x98)[0]
            self.threads.append(dict(tid=tid, st_start=st_start, st_size=st_size,
                                     st_rva=st_rva, rip=rip, rsp=rsp))

    def mod_of(self, addr):
        for base, size, name in self.mods:
            if base <= addr < base + size:
                short = name.replace("\\", "/").split("/")[-1]
                return f"{short}+0x{addr - base:X}"
        return None

    def read_mem(self, rva, size):
        return self.d[rva: rva + size]


def main():
    path = sys.argv[1]
    scan_tid = None
    if "--scan-tid" in sys.argv:
        scan_tid = int(sys.argv[sys.argv.index("--scan-tid") + 1])
    d = Dump(path)
    print(f"modules={len(d.mods)} threads={len(d.threads)}")
    for t in d.threads:
        m = d.mod_of(t["rip"]) or f"raw 0x{t['rip']:X}"
        mark = ""
        if scan_tid and t["tid"] == scan_tid:
            mark = "  <== SCAN"
        print(f"TID {t['tid']:>6}  RIP {m:<34} rsp=0x{t['rsp']:X}{mark}")
    if scan_tid:
        t = next(x for x in d.threads if x["tid"] == scan_tid)
        data = d.read_va(t["st_start"], t["st_size"])
        lo, hi = t["st_start"], t["st_start"] + t["st_size"]
        print(f"\n== stack scan TID {scan_tid} span 0x{lo:X}-0x{hi:X} ({t['st_size']}B) ==")
        hits = []
        for off in range(0, len(data) - 8, 8):
            (v,) = struct.unpack_from("<Q", data, off)
            if lo <= v < hi:
                continue
            m = d.mod_of(v)
            if m:
                hits.append((off, v, m))
        # 压缩连续重复，打印全部模块命中
        prev = None
        for off, v, m in hits:
            if m.split("+")[0] == prev and off and hits and False:
                continue
            print(f"  +0x{off:<5X} -> {m}")
            prev = m.split("+")[0]


if __name__ == "__main__":
    main()
