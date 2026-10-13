#!/usr/bin/env python3
"""GENA 订阅链路端到端验证（SUBSCRIBE → 初始 NOTIFY → 状态变化 NOTIFY → UNSUBSCRIBE）。

为什么单独有这一个脚本：`dmr_compat_check.py` 只覆盖**轮询**链路（控制点每 ~1s 调
GetTransportInfo/GetPositionInfo）。而 DLNA 规格里进度/状态还有第二条路 —— GENA
事件推送。两条路都要通才算「最大兼容性」：

  * 消费级 App（bilibili、百度 DumediaDLNA）走轮询，从不订阅；
  * 但 Windows「播放到」、部分电视/车机、Kodi 等走 GENA 订阅，收 LastChange 推状态。

本脚本用真实 SUBSCRIBE 打设备的三条 eventSubURL，并起一个本地 HTTP 服务收 NOTIFY，
逐条校验：
  0. **CALLBACK 必须写本机局域网 IP，不能写 127.0.0.1** —— libupnp 的
     `gena_validate_delivery_urls()` 会拿回调主机跟本机接口网段比对，loopback /
     0.0.0.0 / 异网段一律回 **412 Precondition Failed**。这是 libupnp 原生行为
     （抗伪造），真实控制点在别的机器上、自然带自己的局域网 IP，不受影响。
     本脚本会自动挑一个本机非 loopback IPv4 作回调地址。
  1. SUBSCRIBE 必须回 200 + `SID` + `TIMEOUT`；
  2. 必须**立刻**收到 SEQ=0 的初始 NOTIFY（订阅后不发初始事件 = 控制点永远空白）；
  3. AVT/RCS 的 SEQ=0 必须带 `LastChange`，且是一段**转义过的** XML（`&lt;Event...`）；
  4. CM 没有 LastChange，必须推 `SourceProtocolInfo`/`SinkProtocolInfo`/`CurrentConnectionIDs`
     三个变量各自的值（照搬 AVT 那套 = 控制点拿不到 Sink 能力）；
  5. 订阅期间改状态（Pause/Play）必须收到 SEQ=1 的增量 NOTIFY；
  6. UNSUBSCRIBE 回 200。

用法：
  python tests/gena_subscribe_check.py                       # 自动探测端点
  python tests/gena_subscribe_check.py --host 192.0.2.1:49152
退出码 0 = 全绿。
"""
from __future__ import annotations

import argparse
import http.server
import re
import socket
import sys
import threading
import time
import urllib.request

AVT = "urn:schemas-upnp-org:service:AVTransport:1"
RCS = "urn:schemas-upnp-org:service:RenderingControl:1"
CM = "urn:schemas-upnp-org:service:ConnectionManager:1"

SERVICES = {
    "AVT": (AVT, "/fr/event/AVTransport", "/fr/control/AVTransport"),
    "RCS": (RCS, "/fr/event/RenderingControl", "/fr/control/RenderingControl"),
    "CM": (CM, "/fr/event/ConnectionManager", "/fr/control/ConnectionManager"),
}

_fail: list[str] = []
_pass = 0


def check(ok: bool, label: str, detail: str = "") -> None:
    global _pass
    if ok:
        _pass += 1
    else:
        _fail.append(f"{label}{('  ← ' + detail) if detail else ''}")
    print(f"  [{'OK ' if ok else 'FAIL'}] {label}" + (f"  {detail}" if detail and not ok else ""))


class NotifySink(http.server.BaseHTTPRequestHandler):
    """收 NOTIFY 的回调服务。"""

    received: list[tuple[str, str, str]] = []  # (sid, seq, body)
    lock = threading.Lock()

    def do_NOTIFY(self):  # noqa: N802
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n).decode("utf-8", "replace") if n else ""
        sid = self.headers.get("SID", "")
        seq = self.headers.get("SEQ", "")
        with NotifySink.lock:
            NotifySink.received.append((sid, seq, body))
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *a):  # 静音
        pass


def wait_for(pred, timeout: float):
    t0 = time.time()
    while time.time() - t0 < timeout:
        with NotifySink.lock:
            for item in NotifySink.received:
                if pred(item):
                    return item
        time.sleep(0.1)
    return None


def http_call(method: str, host: str, path: str, headers: dict[str, str]) -> tuple[int, dict[str, str]]:
    ip, port = host.rsplit(":", 1)
    s = socket.create_connection((ip, int(port)), timeout=8)
    req = f"{method} {path} HTTP/1.1\r\nHost: {host}\r\n"
    for k, v in headers.items():
        req += f"{k}: {v}\r\n"
    req += "Content-Length: 0\r\nConnection: close\r\n\r\n"
    s.sendall(req.encode())
    buf = b""
    while True:
        b = s.recv(65536)
        if not b:
            break
        buf += b
    s.close()
    text = buf.decode("utf-8", "replace")
    status = int(re.match(r"HTTP/1\.\d (\d+)", text).group(1))
    hdrs = {}
    for line in text.split("\r\n\r\n", 1)[0].split("\r\n")[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            hdrs[k.strip().lower()] = v.strip()
    return status, hdrs


def soap_action(host: str, path: str, svc: str, action: str, inner: str) -> int:
    env = (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
        's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
        f'<s:Body><u:{action} xmlns:u="{svc}">{inner}</u:{action}></s:Body></s:Envelope>'
    ).encode()
    ip, port = host.rsplit(":", 1)
    s = socket.create_connection((ip, int(port)), timeout=8)
    head = (
        f"POST {path} HTTP/1.1\r\nHost: {host}\r\n"
        'Content-Type: text/xml; charset="utf-8"\r\n'
        f'SOAPACTION: "{svc}#{action}"\r\n'
        f"Content-Length: {len(env)}\r\nConnection: close\r\n\r\n"
    ).encode()
    s.sendall(head + env)
    buf = b""
    while True:
        b = s.recv(65536)
        if not b:
            break
        buf += b
    s.close()
    m = re.match(rb"HTTP/1\.\d (\d+)", buf)
    return int(m.group(1)) if m else 0


def local_lan_ip(peer_ip: str) -> str:
    """挑一个本机非 loopback 的 IPv4（用于 CALLBACK，见文件头第 0 条）。

    用 UDP connect 到对端，内核会挑出「到那个对端实际会用的」本机源地址 ——
    比枚举网卡再猜更可靠（多网卡 / VPN 环境尤其如此）。
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((peer_ip, 9))
        return s.getsockname()[0]
    except OSError:
        return ""
    finally:
        s.close()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="")
    ap.add_argument("--cb-host", default="", help="覆盖回调主机（默认自动取本机局域网 IP）")
    ap.add_argument("--timeout", type=float, default=8.0, help="等初始 NOTIFY 的秒数")
    args = ap.parse_args()

    host = args.host
    if not host:
        for cand in ("127.0.0.1:49152", "192.0.2.1:49152"):
            try:
                ip, port = cand.rsplit(":", 1)
                socket.create_connection((ip, int(port)), timeout=3).close()
                host = cand
                break
            except OSError:
                continue
    if not host:
        print("找不到可用设备端点（--host ip:port）")
        return 2

    peer_ip, _ = host.rsplit(":", 1)
    cb_host = args.cb_host or local_lan_ip(peer_ip)
    if not cb_host:
        print("无法确定本机局域网 IP —— GENA 回调必须用非 loopback 地址，请用 --cb-host 指定")
        return 2

    # 回调服务：绑 0.0.0.0 便于设备回连
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), NotifySink)
    cb_port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print(f"### 设备 {host}  回调 host={cb_host}:{cb_port}\n")

    subs: list[tuple[str, str, str]] = []  # (key, svc, sid)
    for key, (svc, evpath, _) in SERVICES.items():
        print(f"{key}  ({evpath})")
        status, hdrs = http_call("SUBSCRIBE", host, evpath, {
            "CALLBACK": f"<http://{cb_host}:{cb_port}/cb>",
            "NT": "upnp:event",
            "TIMEOUT": "Second-1800",
        })
        sid = hdrs.get("sid", "")
        check(status == 200, f"{key} SUBSCRIBE 回 200", f"HTTP {status}")
        check(bool(sid) and sid.startswith("uuid:"), f"{key} SUBSCRIBE 回 SID", f"SID={sid!r}")
        check("Timeout" in str(hdrs.get("timeout", "")) or bool(hdrs.get("timeout")),
              f"{key} SUBSCRIBE 回 TIMEOUT", f"TIMEOUT={hdrs.get('timeout')!r}")
        if sid:
            subs.append((key, svc, sid))

        # 初始 NOTIFY（SEQ=0）
        item = wait_for(lambda it: it[0] == sid and it[1] == "0", args.timeout)
        check(item is not None, f"{key} 订阅后立即收到 SEQ=0 初始 NOTIFY",
              f"{args.timeout}s 内未收到")
        if item:
            body = item[2]
            if key == "CM":
                for var in ("SourceProtocolInfo", "SinkProtocolInfo", "CurrentConnectionIDs"):
                    check(f"<{var}>" in body, f"CM 初始 NOTIFY 含变量 {var}",
                          f"body={body[:160]}")
            else:
                check("<LastChange>" in body, f"{key} 初始 NOTIFY 含 LastChange",
                      f"body={body[:160]}")
                m = re.search(r"<LastChange>(.*?)</LastChange>", body, re.S)
                inner = m.group(1) if m else ""
                check("&lt;Event" in inner, f"{key} LastChange 是转义过的 XML",
                      f"inner={inner[:120]!r}")
        print()

    # ---- 状态变化必须触发 SEQ=1 增量 NOTIFY ----
    print("G. 状态变化触发增量事件")
    if subs:
        avt_sid = next((s for k, _, s in subs if k == "AVT"), "")
        path = SERVICES["AVT"][2]
        before = len(NotifySink.received)
        soap_action(host, path, AVT, "Pause", "<InstanceID>0</InstanceID>")
        item = wait_for(lambda it: it[0] == avt_sid and it[1] == "1", 6.0)
        if item is None:
            # 设备可能不在 PLAYING：再试 Play
            soap_action(host, path, AVT, "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>")
            item = wait_for(lambda it: it[0] == avt_sid and it[1] == "1", 6.0)
        check(item is not None, "AVT 状态变化 → 收到 SEQ=1 增量 NOTIFY",
              f"新增 {len(NotifySink.received) - before} 条通知但无 SEQ=1")
        if item:
            body = item[2]
            m = re.search(r"<LastChange>(.*?)</LastChange>", body, re.S)
            inner = (m.group(1) if m else "").replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", '"')
            check("TransportState" in inner, "增量 NOTIFY 的 LastChange 含 TransportState",
                  f"inner={inner[:200]}")

    # ---- UNSUBSCRIBE ----
    print("\nH. UNSUBSCRIBE")
    for key, _, sid in subs:
        status, _ = http_call("UNSUBSCRIBE", host, SERVICES[key][1], {"SID": sid})
        check(status == 200, f"{key} UNSUBSCRIBE 回 200", f"HTTP {status}")

    srv.shutdown()
    print(f"\n===== 通过 {_pass} / 失败 {len(_fail)} =====")
    for f in _fail:
        print(f"  ✗ {f}")
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
