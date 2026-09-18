#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""多网卡支持验收脚本（v0.5.0 / P2 + P3 + P4 + P5）。

为什么需要它：多网卡改造的验收点是「**每个网段各发一次 M-SEARCH，各自只收到指向
本网段 IP 的 LOCATION**」。一台机器上的多个网段（物理网卡 + Hyper-V 交换机 +
VMware 虚拟网卡）完全可以本机自测：把 UDP 套接字**绑到指定的源 IP** 发 M-SEARCH，
协议栈就会按该来源地址判定对端网段 —— 等于模拟了不同网段上的控制点。

用法：
    python tests/multi_nic_check.py                 # 自动枚举本机 IPv4，逐个网段自测
    python tests/multi_nic_check.py --src 192.0.2.1 --src 192.0.2.2
    python tests/multi_nic_check.py --port 49152
    python tests/multi_nic_check.py --expect-all 192.0.2.2   # 负向对照（旧版行为）
    python tests/multi_nic_check.py --watch 192.0.2.2        # 单网卡收 NOTIFY
    python tests/multi_nic_check.py --watch-all                 # P5：所有网卡收 NOTIFY

判定标准：
    · 每个来源网段的 M-SEARCH 都能收到回复；
    · 回复里的 LOCATION 主机段 == **该来源网段的本机 IP**；
    · 回复 LOCATION 指向的设备描述与 SCPD 都能 HTTP 200 取到（HTTP 已绑 0.0.0.0）；
    · URLBase 主机段 == 本请求的 Host（d117：谁问就按谁回，不再有"优选网卡"档）；
    · ALIVE/BYEBYE 在**每一块**网卡上各发一份，LOCATION 各指各的（P5）。

退出码 0 = 全部通过；非 0 = 有网段失败（逐条打印原因）。
"""

from __future__ import annotations

import argparse
import select
import socket
import sys
import time
import urllib.request
from urllib.parse import urlparse

SSDP_ADDR = "239.255.255.250"
SSDP_PORT = 1900

M_SEARCH = (
    "M-SEARCH * HTTP/1.1\r\n"
    f"HOST: {SSDP_ADDR}:{SSDP_PORT}\r\n"
    'MAN: "ssdp:discover"\r\n'
    "MX: 1\r\n"
    "ST: upnp:rootdevice\r\n"
    "\r\n"
).encode("ascii")


def local_ipv4() -> list[str]:
    """枚举本机 IPv4。gethostname+getaddrinfo 在 Win/Linux 上都能列出全部本机地址。"""
    try:
        infos = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
    except OSError as exc:
        print(f"[warn] 自动枚举本机 IPv4 失败：{exc}", file=sys.stderr)
        return []
    return sorted({i[4][0] for i in infos})


def parse_headers(text: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in text.split("\r\n")[1:]:
        if ":" in line:
            k, _, v = line.partition(":")
            out[k.strip().lower()] = v.strip()
    return out


def m_search(src_ip: str, wait: float) -> list[tuple[str, dict[str, str]]]:
    """把套接字绑到 src_ip 发 M-SEARCH，返回 [(来源地址, 头字典)]。"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((src_ip, 0))  # 关键：源地址 = 模拟的网段
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
    try:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    except OSError:
        pass
    # 显式指定出接口，保证多播从该来源网卡出去（也让本机监听端按该网卡收到）
    try:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                        socket.inet_aton(src_ip))
    except OSError as exc:
        print(f"[warn] {src_ip} 设置 IP_MULTICAST_IF 失败：{exc}", file=sys.stderr)

    sock.settimeout(wait)
    sock.sendto(M_SEARCH, (SSDP_ADDR, SSDP_PORT))

    replies: list[tuple[str, dict[str, str]]] = []
    deadline = time.monotonic() + wait
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            break
        sock.settimeout(left)
        try:
            data, addr = sock.recvfrom(65535)
        except socket.timeout:
            break
        except OSError:
            break
        replies.append((addr[0], parse_headers(data.decode("utf-8", "replace"))))
    sock.close()
    return replies


def http_status(url: str, timeout: float = 3.0) -> str:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            return f"HTTP {resp.status}"
    except urllib.error.HTTPError as exc:
        return f"HTTP {exc.code}"
    except Exception as exc:  # noqa: BLE001 - 报告比抛栈有用
        return f"失败（{type(exc).__name__}）"


def watch_notify(iface_ip: str, seconds: float) -> int:
    """在 iface_ip 上加入 SSDP 组播组，被动收 NOTIFY（ssdp:alive / byebye）。

    为什么要单独看这个：M-SEARCH 是「有客户端地址」的场景，可以按对端网段选本机 IP；
    而 ALIVE/BYEBYE 是设备**主动**向 239.255.255.250 广播的，**没有对端可匹配** ——
    这是唯一必须"挑一块网卡发"的路径，因此要单独确认它的 LOCATION 主机段正确。
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind((iface_ip, SSDP_PORT))
    except OSError as exc:
        print(f"[FAIL] 无法在 {iface_ip}:{SSDP_PORT} 监听：{exc}", file=sys.stderr)
        return 2
    mreq = socket.inet_aton(SSDP_ADDR) + socket.inet_aton(iface_ip)
    try:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    except OSError as exc:
        print(f"[FAIL] {iface_ip} 加入 {SSDP_ADDR} 失败：{exc}", file=sys.stderr)
        sock.close()
        return 2
    print(f"在 {iface_ip}:{SSDP_PORT} 监听 NOTIFY（{seconds:.0f} 秒）…")

    sock.settimeout(seconds)
    deadline = time.monotonic() + seconds
    alive = 0
    wrong = 0
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            break
        sock.settimeout(left)
        try:
            data, addr = sock.recvfrom(65535)
        except socket.timeout:
            break
        except OSError:
            break
        text = data.decode("utf-8", "replace")
        if not text.upper().startswith("NOTIFY"):
            print(f"  [非 NOTIFY] 来自 {addr[0]}: {text.splitlines()[0]!r}")
            continue
        hdrs = parse_headers(text)
        loc = hdrs.get("location", "(无 LOCATION)")
        host = urlparse(loc).hostname or ""
        ours = host == iface_ip
        if "alive" in hdrs.get("nts", "").lower():
            alive += 1
            if not ours:
                wrong += 1
            print(f"  [{'OK  ' if ours else 'FAIL'}] ssdp:alive  LOCATION={loc}")
            print(f"          来源={addr[0]}  NT={hdrs.get('nt', '?')}")
        else:
            # byebye 通常不带 LOCATION；带了也要对
            print(f"  [{'-' }] {hdrs.get('nts', '?')}  LOCATION={loc}  来源={addr[0]}")
    sock.close()

    print(f"\n  小结：收到 {alive} 条 ssdp:alive，其中主机段不等于 {iface_ip} 的 {wrong} 条")
    if alive == 0:
        print(f"  [WARN] {seconds:.0f} 秒内没收到 alive —— 可能设备已过开机广播窗口"
              f"（ALIVE 是启动时与周期性重发的，重跑请在启动设备后立刻执行）")
        return 0
    return 1 if wrong else 0


def watch_all(ifaces: list[str], seconds: float) -> int:
    """在**每一块**网卡上同时加入 SSDP 组播组，收 NOTIFY —— 验收 d117 的核心承诺。

    主动广播（ALIVE/BYEBYE）没有对端可匹配，d117 之前的实现"回退优选网卡"：
    只在物理网卡上发一份，其余网段上的控制点**永远收不到**（不是晚点收到）。
    d117 改成逐张网卡各发一份，每份 LOCATION 主机段 = 该网卡自己的 IP。
    所以正确状态是：每一块网卡都该收到 alive，且每条的 LOCATION 主机段都等于
    那块网卡的 IP。任何一块收不到、或收到的是别的网卡 IP，都算失败。
    """
    socks: list[tuple[str, socket.socket]] = []
    for ip in ifaces:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((ip, SSDP_PORT))
            mreq = socket.inet_aton(SSDP_ADDR) + socket.inet_aton(ip)
            s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
            socks.append((ip, s))
        except OSError as exc:
            print(f"  [FAIL] {ip} 监听/入组失败：{exc}")
            s.close()
    if not socks:
        return 2

    print(f"在 {len(socks)} 块网卡上同时监听 NOTIFY（{seconds:.0f} 秒）…")
    print("（ALIVE 集中在设备启动瞬间 —— 请在本窗口内启动/重启设备）")

    deadline = time.monotonic() + seconds
    stat = {ip: {"alive": 0, "wrong": 0, "other": 0, "hosts": set()} for ip, _ in socks}
    local_set = set(ifaces) | set(local_ipv4())

    while time.monotonic() < deadline:
        rlist, _, _ = select.select([s for _, s in socks], [], [], 0.5)
        if not rlist:
            continue
        for s in rlist:
            iface = next(ip for ip, sk in socks if sk is s)
            try:
                data, addr = s.recvfrom(65535)
            except OSError:
                continue
            text = data.decode("utf-8", "replace")
            if not text.upper().startswith("NOTIFY"):
                continue
            hdrs = parse_headers(text)
            if "alive" not in hdrs.get("nts", "").lower():
                continue
            loc = hdrs.get("location", "")
            host = urlparse(loc).hostname or ""
            if host and host not in local_set:
                stat[iface]["other"] += 1  # 局域网里别的 DLNA 设备，与本验收无关
                continue
            st = stat[iface]
            st["alive"] += 1
            st["hosts"].add(host)
            ours = host == iface
            if not ours:
                st["wrong"] += 1
            print(f"  [{iface}] [{'OK  ' if ours else 'FAIL'}] "
                  f"alive LOCATION={loc}  来源={addr[0]}")

    failed = 0
    print("\n" + "-" * 52)
    for ip, _ in socks:
        st = stat[ip]
        ok = st["alive"] > 0 and st["wrong"] == 0
        if st["alive"] == 0:
            failed += 1
            print(f"  [FAIL] {ip:<15} 没收到任何本机 alive（该网卡没有被广播到）")
        elif st["wrong"]:
            failed += 1
            print(f"  [FAIL] {ip:<15} {st['alive']} 条 alive，其中 {st['wrong']} 条的"
                  f"LOCATION 不指向本网卡：{sorted(st['hosts'])}")
        else:
            print(f"  [ OK ] {ip:<15} {st['alive']} 条 alive，LOCATION 主机段全部 = 本网卡"
                  f"（其他设备 {st['other']} 条已忽略）")
    print("-" * 52)
    print("结果：" + ("失败 —— 主动广播没有覆盖到每一块网卡" if failed
                    else "通过 —— ALIVE 逐张网卡各发一份，LOCATION 各指各的"))
    for ip, s in socks:
        s.close()
    return 1 if failed else 0


def soap_post(url: str, service_type: str, action: str, instance_id: int = 0) -> tuple[str, str]:
    """发一个真实 SOAP 请求，返回 (状态, 响应摘要)。URL 用客户端会用的那个。"""
    body = (
        '<?xml version="1.0" encoding="utf-8"?>\r\n'
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
        's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">\r\n'
        f"<s:Body><u:{action} xmlns:u=\"{service_type}\">"
        f"<InstanceID>{instance_id}</InstanceID>"
        f"</u:{action}></s:Body></s:Envelope>\r\n"
    ).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=body,
        headers={
            "Content-Type": 'text/xml; charset="utf-8"',
            "SOAPACTION": f'"{service_type}#{action}"',
            "User-Agent": "FlashRec-multi-nic-check/1.0 UPnP/1.0",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=4) as resp:
            text = resp.read().decode("utf-8", "replace")
            return f"HTTP {resp.status}", text
    except urllib.error.HTTPError as exc:
        return f"HTTP {exc.code}", exc.read().decode("utf-8", "replace")[:200]
    except Exception as exc:  # noqa: BLE001
        return f"失败（{type(exc).__name__}: {exc}）", ""


def check_control_plane(src: str, port: int) -> int:
    """按**客户端的方式**走一遍控制面：取描述 → 用 URLBase 解析控制地址 → 发 SOAP。

    这一步专门抓「描述里的 URLBase 写死成别的网卡 IP」这类漏点：控制点拿到正确的
    LOCATION、描述也取到了，但按 URLBase 解析出来的 controlURL 指向另一块网卡 ⇒
    SOAP 打不通 ⇒ 投屏在"发现之后"失败。
    """
    failed = 0
    desc_url = f"http://{src}:{port}/description.xml"
    try:
        with urllib.request.urlopen(desc_url, timeout=4) as resp:
            xml = resp.read()
    except Exception as exc:  # noqa: BLE001
        print(f"  [FAIL] 取描述失败 {desc_url}：{exc}")
        return 1

    import re
    m = re.search(rb"<URLBase>\s*([^<\s]+)\s*</URLBase>", xml)
    url_base = m.group(1).decode() if m else ""
    host = urlparse(url_base).hostname or ""
    # d117 起的合同：URLBase 主机段 = **本请求的 Host 头**（即客户端用来找到我们的
    # 地址）。客户端拉描述用的 URL 就是 http://<src>:<port>/description.xml，
    # 所以 URLBase 必须等于 src —— 这正是"谁问就按谁回"，不再有"优选网卡"这一档。
    ok_base = host == src
    if not ok_base:
        failed += 1
    print(f"  [{'OK  ' if ok_base else 'FAIL'}] URLBase={url_base or '(无)'}"
          f"（必须 = 本网段 {src}，实际={host or '?'}）")
    if ok_base and src not in set(local_ipv4()):
        print(f"        注：{src} 不在本机地址里，URLBase 却等于它 —— 异常，需排查")

    controls = re.findall(rb"<controlURL>\s*([^<\s]+)\s*</controlURL>", xml)
    if not controls:
        print("  [FAIL] 描述里没有 controlURL")
        return failed + 1
    tests = [
        (b"/fr/control/AVTransport", "urn:schemas-upnp-org:service:AVTransport:1", "GetTransportInfo"),
        (b"/fr/control/RenderingControl", "urn:schemas-upnp-org:service:RenderingControl:1", "GetVolume"),
        (b"/fr/control/ConnectionManager", "urn:schemas-upnp-org:service:ConnectionManager:1",
         "GetProtocolInfo"),
    ]
    for path, svc, action in tests:
        if path not in controls:
            print(f"  [FAIL] 描述里缺 controlURL {path.decode()}")
            failed += 1
            continue
        # 客户端就是这么解析的：相对地址按 URLBase（缺失则按描述 URL）拼
        url = urllib.parse.urljoin(url_base or desc_url, path.decode())
        status, text = soap_post(url, svc, action)
        ok = status.startswith("HTTP 200") and "Fault" not in text
        if not ok:
            failed += 1
        print(f"  [{'OK  ' if ok else 'FAIL'}] {action:<18} -> {status}  {url}")
        if ok:
            snippet = re.search(r"<CurrentTransportState>([^<]*)<", text)
            if snippet:
                print(f"        响应：CurrentTransportState={snippet.group(1)}")
    return failed


def main() -> int:
    ap = argparse.ArgumentParser(description="FlashRec 多网卡验收（P2/P3）")
    ap.add_argument("--src", action="append", default=[],
                    help="来源 IP（可重复）；默认自动枚举本机 IPv4")
    ap.add_argument("--port", type=int, default=49152, help="设备 HTTP 端口")
    ap.add_argument("--wait", type=float, default=2.5, help="每次 M-SEARCH 收包时长（秒）")
    ap.add_argument("--expect-all", metavar="IP", default=None,
                    help="负向对照：要求所有网段都返回这个 IP（旧版单接口行为）")
    ap.add_argument("--watch", metavar="IP", default=None,
                    help="只被动监听该网卡上的 NOTIFY（ALIVE/BYEBYE）并校验 LOCATION")
    ap.add_argument("--watch-all", action="store_true",
                    help="P5：在**每一块**网卡上同时收 NOTIFY，验收主动广播逐网卡各发一份"
                         "（先启动本命令，再启动/重启设备，窗口时长用 --wait 控制）")
    args = ap.parse_args()

    if args.watch_all:
        return watch_all(local_ipv4(), max(args.wait, 20.0))
    if args.watch:
        return watch_notify(args.watch, max(args.wait, 15.0))

    srcs = args.src or local_ipv4()
    if not srcs:
        print("没有可测的 IPv4 来源地址，用 --src 指定。", file=sys.stderr)
        return 2

    print(f"设备 HTTP 端口={args.port}  测试来源网段：{', '.join(srcs)}")
    if args.expect_all:
        print(f"[负向对照] 期望所有网段都返回 {args.expect_all}（旧版行为）")

    failed = 0

    print("\n=== P2 · HTTP 全接口可达（设备自身）===")
    for ip in srcs:
        print(f"  http://{ip}:{args.port}/description.xml -> "
              f"{http_status(f'http://{ip}:{args.port}/description.xml')}")

    # 本机 IP 集合：用来把「局域网里别的 DLNA 设备」的回复滤掉
    local_set = set(srcs) | set(local_ipv4())

    print("\n=== P3 · 按来源网段返回正确的 LOCATION ===")
    for src in srcs:
        all_replies = m_search(src, args.wait)
        replies = []
        foreign = 0
        for from_ip, hdrs in all_replies:
            host = urlparse(hdrs.get("location", "")).hostname or ""
            if host in local_set:
                replies.append((from_ip, hdrs))
            else:
                foreign += 1
        want = args.expect_all or src
        print(f"\n  [来源 {src}] 收到 {len(all_replies)} 份回复"
              f"（其中本机设备 {len(replies)} 份，局域网其他 DLNA 设备 {foreign} 份已忽略）")
        if not replies:
            failed += 1
            print("    [FAIL] 该网段收不到本机设备的回复（该网卡未加入组播组？）")
            continue
        seen: set[str] = set()
        for from_ip, hdrs in replies:
            loc = hdrs.get("location", "(无 LOCATION)")
            host = urlparse(loc).hostname or ""
            seen.add(host)
            ok = host == want
            if not ok:
                failed += 1
            print(f"    [{'OK  ' if ok else 'FAIL'}] LOCATION={loc}")
            print(f"           来源={from_ip}  ST={hdrs.get('st', '?')}  "
                  f"期望主机段={want}  实际={host or '?'}")
        if len(seen) > 1:
            print(f"    [WARN] 同一来源收到多个不同 LOCATION 主机段：{sorted(seen)}"
                  f"（多 device handle 才会这样，属异常）")
        # 取回复里的描述与 SCPD，确认客户端真能连上（SCPD 路径在 URL 根下）
        loc = replies[0][1].get("location", "")
        if loc:
            base = "{0.scheme}://{0.netloc}".format(urlparse(loc))
            scpd = base + "/fr/scpd/AVTransport.xml"
            print(f"    LOCATION 可达性：{http_status(loc)}   {loc}")
            print(f"    SCPD 可达性：    {http_status(scpd)}   {scpd}")

    print("\n=== P4 · 控制面 SOAP（按客户端的方式：URLBase 解析相对 controlURL）===")
    for src in srcs:
        print(f"\n  [网段 {src}]")
        failed += check_control_plane(src, args.port)

    print("\n" + "-" * 52)
    if failed:
        print(f"结果：失败 {failed} 项 —— 多网卡支持未达成")
        return 1
    print("结果：通过 —— 发现、描述、控制面在每个网段都用本网段 IP")
    return 0


if __name__ == "__main__":
    sys.exit(main())
