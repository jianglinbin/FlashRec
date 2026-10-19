#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""本地更新服务模拟器（v0.6.4 d462；AUTO_UPDATE_PLAN §14 集成测试）。

给真实 `flashrec.exe` 走完整 HTTP 链路：GitHub releases/latest API、
latest.json 清单、资产下载（含 Range 续传）与 releases.atom 兜底。

用法：
    python tests/mock_update_server.py --port 8123 --version 9.9.9 \
        --asset build/mock/FlashRec-9.9.9-win64.zip

把代理指向本机即可让应用走模拟服务（settings.json）：
    "update": {
      "direct_enabled": false,
      "proxies": [{"name": "mock", "prefix": "http://127.0.0.1:8123/", "enabled": true}]
    }

清单四个平台键都会生成，名称随 --version；任何键的下载都返回同一份 --asset
（下载器只校验大小与 SHA-256，不解析安装包内容）。
"""
import argparse
import hashlib
import json
import os
import re
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit

KINDS = ("windows_setup", "windows_zip", "linux_appimage", "linux_deb")


def asset_names(version):
    return {
        "windows_setup": f"FlashRec-{version}-win64-setup.exe",
        "windows_zip": f"FlashRec-{version}-win64.zip",
        "linux_appimage": f"FlashRec-{version}-x86_64.AppImage",
        "linux_deb": f"flashrec_{version}_amd64.deb",
    }


class State:
    def __init__(self, version, repo, asset_path, verbose):
        self.version = version
        self.repo = repo
        self.asset_path = asset_path
        self.verbose = verbose
        self.lock = threading.Lock()
        self.hits = {}
        with open(asset_path, "rb") as f:
            self.asset = f.read()
        self.sha256 = hashlib.sha256(self.asset).hexdigest()
        self.size = len(self.asset)
        self.names = asset_names(version)

    def note(self, path):
        with self.lock:
            self.hits[path] = self.hits.get(path, 0) + 1
        if self.verbose:
            print(f"[mock] {path}", flush=True)

    def api_json(self):
        return {
            "tag_name": f"v{self.version}",
            "html_url": f"https://github.com/{self.repo}/releases/tag/v{self.version}",
            "published_at": "2026-10-19T00:00:00Z",
            "body": "Mock release for integration testing.",
            "prerelease": False,
            "draft": False,
            "assets": [
                {
                    "name": name,
                    "size": self.size,
                    "browser_download_url":
                        f"https://github.com/{self.repo}/releases/download/"
                        f"v{self.version}/{name}",
                }
                for name in self.names.values()
            ],
        }

    def manifest(self):
        return {
            "schema": 1,
            "version": self.version,
            "published_at": "2026-10-19T00:00:00Z",
            "release_url": f"https://github.com/{self.repo}/releases/tag/v{self.version}",
            "notes": "Mock update notes: testing the silent download and ready prompt.",
            "assets": {
                key: {"name": self.names[key], "size": self.size, "sha256": self.sha256}
                for key in KINDS
            },
        }

    def atom(self):
        return (
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<feed xmlns="http://www.w3.org/2005/Atom">\n'
            f"<entry><title>v{self.version}</title>"
            f"<link href=\"https://github.com/{self.repo}/releases/tag/v{self.version}\"/>"
            "</entry>\n</feed>\n"
        ).encode()


class Handler(BaseHTTPRequestHandler):
    state: State = None  # type: ignore

    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body, headers=None, range_header=None):
        headers = dict(headers or {})
        start = 0
        total = len(body)
        if range_header:
            m = re.match(r"bytes=(\d+)-$", range_header.strip())
            if m:
                start = int(m.group(1))
                if start >= total:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{total}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                self.send_response(206)
                self.send_header("Content-Range", f"bytes {start}-{total - 1}/{total}")
        else:
            self.send_response(code)
        self.send_header("Content-Length", str(total - start))
        self.send_header("Accept-Ranges", "bytes")
        for k, v in headers.items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body[start:])

    def do_GET(self):
        path = unquote(urlsplit(self.path).path)
        self.state.note(path)
        if path.endswith("/latest.json"):
            body = json.dumps(self.state.manifest(), ensure_ascii=False).encode()
            return self._send(200, body, {"Content-Type": "application/json"})
        if path.endswith("/releases/latest"):
            body = json.dumps(self.state.api_json(), ensure_ascii=False).encode()
            return self._send(200, body, {"Content-Type": "application/json"})
        if path.endswith("/releases.atom"):
            return self._send(200, self.state.atom(), {"Content-Type": "application/atom+xml"})
        name = path.rsplit("/", 1)[-1]
        if name in self.state.names.values():
            return self._send(200, self.state.asset, {"Content-Type": "application/octet-stream"},
                              self.headers.get("Range"))
        self._send(404, b"not found", {"Content-Type": "text/plain"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8123)
    ap.add_argument("--version", default="9.9.9")
    ap.add_argument("--repo", default="jianglinbin/FlashRec")
    ap.add_argument("--asset", required=True, help="用作安装包响应的本地文件")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    if not os.path.isfile(args.asset):
        sys.exit(f"资产不存在：{args.asset}")

    Handler.state = State(args.version, args.repo, args.asset, not args.quiet)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"[mock] listening on http://127.0.0.1:{args.port} "
          f"(v{args.version}, asset {args.asset}, {Handler.state.size} B)", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
