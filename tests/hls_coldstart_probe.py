#!/usr/bin/env python3
"""hls_coldstart_probe.py —— 微信 HLS 直播 m3u8 冷启动独立探针（★d512 取证配套）。

目的：用**纯 socket**（不经 FlashRec / mpv / libcurl）独立计时一次直播 m3u8 请求的
DNS / TCP 连接 / 首字节（TTFB）/ 下载完成耗时，判定「起播 ~7s 卡在服务端还是客户端」。

两种用法：
  1) watch 模式（推荐）：python hls_coldstart_probe.py --watch <flashrec.log>
     监听日志，一出现新的 m3u8 SetURI 立即抓 URL 开抢（与 mpv 的请求赛跑），
     然后按 --interval 周期复测到 --duration 结束。
  2) 手动模式：python hls_coldstart_probe.py --url <m3u8地址> [--interval ...]

输出每行一次请求：seq / dns / connect / ttfb / total / 状态码 / 字节数。
判读：
  · 首次 ttfb ~7s、后续骤降  → 服务端冷启动（源站/转 HLS 攒片），客户端无责
  · 全程 ttfb 毫秒级         → 服务端不慢，慢在 mpv 侧（回 mpv_bridge 查）
  · dns/connect 大           → 本机解析/网络路径问题
"""
import argparse
import re
import socket
import sys
import time
from urllib.parse import urlsplit


def fetch_once(url: str, timeout: float) -> dict:
    u = urlsplit(url)
    host = u.hostname
    port = u.port or 80
    path = u.path + (("?" + u.query) if u.query else "")
    t0 = time.perf_counter()
    ai = socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)
    t_dns = time.perf_counter() - t0

    t1 = time.perf_counter()
    s = socket.socket(ai[0][0], socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(ai[0][4])
    t_conn = time.perf_counter() - t1

    req = (f"GET {path} HTTP/1.1\r\nHost: {host}\r\n"
           f"User-Agent: fr-coldstart-probe\r\nAccept: */*\r\nConnection: close\r\n\r\n")
    t2 = time.perf_counter()
    s.sendall(req.encode())
    first = s.recv(65536)
    t_ttfb = time.perf_counter() - t2
    body = first
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        body += chunk
    s.close()
    t_total = time.perf_counter() - t0
    line1 = body.split(b"\r\n", 1)[0].decode("latin1", "replace")
    return {"dns": t_dns, "conn": t_conn, "ttfb": t_ttfb, "total": t_total,
            "status": line1[:40], "size": len(body)}


def run(url: str, interval: float, duration: float, tag: str) -> None:
    print(f"[{tag}] 开始探测 {duration:.0f}s，间隔 {interval:.2f}s：{url[:100]}…", flush=True)
    t_end = time.time() + duration
    seq = 0
    while time.time() < t_end:
        seq += 1
        try:
            r = fetch_once(url, timeout=20.0)
            print(f"[{tag}] #{seq:03d} dns={r['dns']*1000:7.1f}ms conn={r['conn']*1000:7.1f}ms "
                  f"ttfb={r['ttfb']*1000:8.1f}ms total={r['total']*1000:8.1f}ms "
                  f"{r['status']} bytes={r['size']}", flush=True)
        except Exception as e:  # noqa: BLE001 —— 探针不允许中断
            print(f"[{tag}] #{seq:03d} 异常: {type(e).__name__}: {e}", flush=True)
        time.sleep(interval)


def watch_log(log_path: str, interval: float, duration: float) -> None:
    print(f"[watch] 监听 {log_path}，等新 m3u8 SetURI …（请现在投屏）", flush=True)
    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        f.seek(0, 2)  # 跳到文件尾，只看新增
        rx = re.compile(r"loadfile (\S+)")
        url = None
        deadline = time.time() + 600  # 最多等 10 分钟
        while url is None and time.time() < deadline:
            line = f.readline()
            if not line:
                time.sleep(0.2)
                continue
            m = rx.search(line)
            if m and ".m3u8" in m.group(1).lower():
                url = m.group(1)
        if url is None:
            print("[watch] 600s 内未等到 m3u8 SetURI，退出", flush=True)
            return
        print(f"[watch] 抓到 URL（自日志出现即刻开抢）", flush=True)
        run(url, interval, duration, "probe")


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--url")
    p.add_argument("--watch", metavar="LOG")
    p.add_argument("--interval", type=float, default=1.0)
    p.add_argument("--duration", type=float, default=90.0)
    a = p.parse_args()
    if a.watch:
        watch_log(a.watch, a.interval, a.duration)
    elif a.url:
        run(a.url, a.interval, a.duration, "probe")
    else:
        p.error("需要 --url 或 --watch <log>")
    return 0 if sys.exc_info()[0] is None else 1


if __name__ == "__main__":
    sys.exit(main())
