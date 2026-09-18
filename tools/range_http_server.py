#!/usr/bin/env python3
"""带 HTTP Range 支持的极简静态文件服务器（本地投屏测试源）。

为什么需要它：Python 标准库的 `http.server.SimpleHTTPRequestHandler` **不支持 Range**，
对 `Range: bytes=N-` 一律返回 200 + 整个文件。而绝大多数 mp4 的 `moov`（索引 atom）在
文件尾部，播放器必须先发 Range 请求跳到尾部读索引再决定能不能播。用 SimpleHTTP 当源，
mpv 会在 `file-loaded` 后 ~25ms 直接报错退出：

    [MPV] loadfile http://127.0.0.1:8766/long.mp4 start=0 rc=0
    [DMR] file-loaded：isLive=true dur=0
    [DMR] mpv error → STOPPED（不再重连）

这个现象是**测试源的缺陷，不是 FlashRec 的缺陷**——用 CDN（bilibili/百度）做源时
Range 正常，从不出现。本脚本就是为消除这个假阴性而写的。

用法：
  python tools/range_http_server.py [--port 8767] [--dir tools/archive]
  curl -r 0-99 -sI http://127.0.0.1:8767/long.mp4   # 应回 206 且带 Content-Range
"""
from __future__ import annotations

import argparse
import functools
import http.server
import os
import re
import socketserver


RANGE_RE = re.compile(r"bytes=(\d*)-(\d*)")


class RangeHandler(http.server.SimpleHTTPRequestHandler):
    """在 SimpleHTTP 基础上补 206 Partial Content（支持单段 Range）。"""

    protocol_version = "HTTP/1.1"  # 必须 1.1，否则 keep-alive/分块下载行为异常

    def send_head(self):  # noqa: D102
        path = self.translate_path(self.path)
        if os.path.isdir(path):
            return super().send_head()
        try:
            f = open(path, "rb")
        except OSError:
            self.send_error(404, "File not found")
            return None

        fs = os.fstat(f.fileno())
        total = fs.st_size
        ctype = self.guess_type(path)
        rng = self.headers.get("Range")
        m = RANGE_RE.fullmatch(rng.strip()) if rng else None

        if not m:
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(total))
            self.send_header("Accept-Ranges", "bytes")
            # DLNA/播放器友好：带上协议信息头
            self.send_header("transferMode.dlna.org", "Streaming")
            self.send_header("contentFeatures.dlna.org",
                             "DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000")
            self.end_headers()
            return f

        start = int(m.group(1)) if m.group(1) else 0
        if m.group(2):
            end = min(int(m.group(2)), total - 1)
        else:
            end = total - 1
        if start > end or start >= total:
            f.close()
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{total}")
            self.end_headers()
            return None

        self.send_response(206)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("transferMode.dlna.org", "Streaming")
        self.send_header("contentFeatures.dlna.org",
                         "DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000")
        self.end_headers()
        f.seek(start)
        self._end = end
        return f

    def copyfile(self, source, outputfile):  # noqa: D102
        end = getattr(self, "_end", None)
        if end is None:
            return super().copyfile(source, outputfile)
        remaining = end - source.tell() + 1
        while remaining > 0:
            chunk = source.read(min(64 * 1024, remaining))
            if not chunk:
                break
            outputfile.write(chunk)
            remaining -= len(chunk)

    def log_message(self, fmt, *args):  # noqa: D102
        print(f"  [src] {self.address_string()} {fmt % args}", flush=True)


class ThreadingServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8767)
    ap.add_argument("--dir", default="tools/archive")
    ap.add_argument("--bind", default="0.0.0.0")
    args = ap.parse_args()

    handler = functools.partial(RangeHandler, directory=args.dir)
    with ThreadingServer((args.bind, args.port), handler) as httpd:
        print(f"Range 源已启动: http://{args.bind}:{args.port}/  ← {os.path.abspath(args.dir)}", flush=True)
        httpd.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
