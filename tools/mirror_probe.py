#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mirror_probe.py —— 自动更新镜像（代理）站点可用性探测。

对应 AUTO_UPDATE_PLAN.md §3.4：update.mirror 候选站在配置进产品前，
先在此验证「真实可用」——不是 URL 能 ping 通，而是：
  1) JSON：能否拿到 releases/latest（直连 + 各镜像转发 API 两种路径）；
  2) 下载：能否经镜像拉下 Release 资产（SHA256SUMS.txt 全量 + 最小安装包
     Range 试速 / --full 全量）；
  3) 完整性：跨来源比对 SHA256SUMS.txt 内容必须逐字节一致（防镜像篡改/损坏），
     资产哈希必须命中清单（--full 或 <3MB 的小资产自动全量校验）。

用法：
  python tools/mirror_probe.py                      # 默认候选 + 默认仓库
  python tools/mirror_probe.py --tag v0.6.3        # API 不可达时的兜底 tag
  python tools/mirror_probe.py --full              # 全量下载最小安装包并验哈希
  python tools/mirror_probe.py --proxy http://127.0.0.1:7890
  python tools/mirror_probe.py --mirror https://ghfast.top/ --mirror https://gh-proxy.com/

退出码：0 = 至少一个镜像通过全部门槛；1 = 全部不可用；2 = 参数/环境错误。
仅用标准库，无第三方依赖。
"""

import argparse
import hashlib
import json
import re
import sys
import time
import urllib.error
import urllib.request

REPO_DEFAULT = "jianglinbin/FlashRec"

# 常见 ghproxy 系镜像（2026-10 快照，失效属常态——这正是本脚本存在的意义）
MIRRORS_DEFAULT = [
    "https://ghfast.top/",
    "https://gh-proxy.com/",
    "https://ghproxy.net/",
    "https://ghproxy.cc/",
    "https://gh.idayer.com/",
]

# 直连 API 不可达时的兜底资产名（v0.6.3 实测清单，d447 台账）
FALLBACK_TAG = "v0.6.3"
FALLBACK_ASSETS = [
    "SHA256SUMS.txt",
    "flashrec_{tag#1}_amd64.deb",
    "FlashRec-{tag#1}-win64.zip",
    "FlashRec-{tag#1}-win64-setup.exe",
    "FlashRec-{tag#1}-x86_64.AppImage",
]

UA = "FlashRec-mirror-probe/0.1"


def http_get(url, timeout, proxy=None, headers=None, max_bytes=None):
    """GET，返回 (status, bytes, elapsed_sec, final_url)。4xx/5xx 抛 urllib.error.HTTPError。"""
    handlers = []
    if proxy:
        handlers.append(urllib.request.ProxyHandler({"http": proxy, "https": proxy}))
    else:
        handlers.append(urllib.request.ProxyHandler({}))  # 显式禁环境代理，测的是裸连通性
    opener = urllib.request.build_opener(*handlers)
    req = urllib.request.Request(url, headers={"User-Agent": UA, **(headers or {})})
    t0 = time.monotonic()
    with opener.open(req, timeout=timeout) as resp:
        data = resp.read(max_bytes) if max_bytes else resp.read()
        return resp.status, data, time.monotonic() - t0, resp.geturl()


def probe_json(url, timeout, proxy=None):
    """探测 JSON 可用性，返回 (ok, tag_name|None, latency, note)。"""
    try:
        status, data, dt, _ = http_get(url, timeout, proxy)
        obj = json.loads(data.decode("utf-8", "replace"))
        tag = obj.get("tag_name")
        if not tag:
            return False, None, dt, f"HTTP {status} 但 JSON 无 tag_name"
        return True, tag, dt, f"HTTP {status}, tag={tag}"
    except urllib.error.HTTPError as e:
        return False, None, 0.0, f"HTTP {e.code}"
    except Exception as e:  # 超时/SSL/DNS/连接拒绝
        return False, None, 0.0, f"{type(e).__name__}: {e}"[:80]


def asset_url(mirror, browser_url):
    """按 ghproxy 系惯例拼 URL：mirror + 完整原始 URL。mirror 为空 = 直连。"""
    return browser_url if not mirror else mirror.rstrip("/") + "/" + browser_url


def parse_sums(text):
    """解析 sha256sum 格式：hexdigest + 两空格 + 文件名。"""
    out = {}
    for line in text.splitlines():
        m = re.match(r"^([0-9a-fA-F]{64})\s+\*?(.+)$", line.strip())
        if m:
            out[m.group(2).strip()] = m.group(1).lower()
    return out


def speed_note(nbytes, dt):
    if dt <= 0:
        return "-"
    return f"{nbytes / dt / 1024:.0f} KB/s"


def main():
    ap = argparse.ArgumentParser(description="自动更新镜像站点可用性探测")
    ap.add_argument("--repo", default=REPO_DEFAULT)
    ap.add_argument("--tag", help="API 不可达时的兜底 tag（默认 %s）" % FALLBACK_TAG)
    ap.add_argument("--asset", help="指定测速资产名（默认自动选最小安装包）")
    ap.add_argument("--full", action="store_true", help="全量下载测速资产并校验哈希（默认仅 <3MB 资产全量）")
    ap.add_argument("--range-bytes", type=int, default=262144, help="试速下载字节数（默认 256KB）")
    ap.add_argument("--timeout", type=int, default=15)
    ap.add_argument("--proxy", help="经本地代理直连 GitHub（如 http://127.0.0.1:7890）")
    ap.add_argument("--mirror", action="append", dest="mirrors", help="覆盖候选镜像（可重复）")
    args = ap.parse_args()

    mirrors = args.mirrors if args.mirrors else MIRRORS_DEFAULT
    api_url = f"https://api.github.com/repos/{args.repo}/releases/latest"
    results = []  # (name, [(gate, ok, note)])

    print("=" * 72)
    print(f" FlashRec 镜像探测  repo={args.repo}  timeout={args.timeout}s"
          + (f"  proxy={args.proxy}" if args.proxy else "  直连(无代理)"))
    print("=" * 72)

    # ---- 1) JSON：直连 API ----
    print("\n[1] JSON 检测：releases/latest")
    ok, tag, dt, note = probe_json(api_url, args.timeout, args.proxy)
    print(f"    直连 api.github.com      : {'OK ' if ok else 'FAIL'} {note}"
          + (f" ({dt*1000:.0f}ms)" if ok else ""))
    assets = None
    if ok:
        try:
            _, data, _, _ = http_get(api_url, args.timeout, args.proxy)
            rel = json.loads(data)
            assets = {a["name"]: a["browser_download_url"] for a in rel.get("assets", [])}
        except Exception:
            pass

    if not ok and not args.tag:
        args.tag = FALLBACK_TAG
        print(f"    直连不可达 → 兜底 tag={args.tag}（可用 --tag 覆盖）")

    # ---- 2) JSON：经镜像转发 API（部分镜像支持，加分项非门槛）----
    print("[2] JSON 检测：镜像转发 API（ghproxy 系多数不支持，属加分项）")
    api_via = {}
    for m in mirrors:
        ok2, _, dt2, note2 = probe_json(asset_url(m, api_url), args.timeout, args.proxy)
        api_via[m] = ok2
        print(f"    {m:<32}: {'OK ' if ok2 else '--'} {note2}")

    # ---- 3) 资产清单与基准哈希 ----
    tag_final = (ok and tag) or args.tag
    ver = tag_final.lstrip("v")
    if assets:
        names = sorted(assets)
    else:
        names = [n.replace("{tag#1}", ver) for n in FALLBACK_ASSETS]
        assets = {n: f"https://github.com/{args.repo}/releases/download/{tag_final}/{n}" for n in names}
    print(f"\n[3] 资产清单（tag={tag_final}）：" + ", ".join(names))

    pick = args.asset
    sizes = _asset_sizes(api_url, args.timeout, args.proxy) if ok else {}
    if not pick:
        cand = [n for n in names if n != "SHA256SUMS.txt"]
        if sizes:
            pick = min(cand, key=lambda n: sizes.get(n, 1 << 62))
        else:
            deb = [n for n in cand if n.endswith(".deb")]
            pick = deb[0] if deb else cand[0]
    print(f"    测速资产：{pick}"
          + (f"（{sizes[pick]/1024/1024:.1f}MB）" if pick in sizes else ""))

    # ---- 4) SHA256SUMS.txt 跨来源一致性 ----
    print("[4] 基准哈希：SHA256SUMS.txt 跨来源比对（篡改/损坏检测）")
    sums_by_src = {}
    sums_url = assets.get("SHA256SUMS.txt")
    sources = [("", sums_url)] + [(m, asset_url(m, sums_url)) for m in mirrors]
    for name, url in sources:
        label = name or "直连"
        try:
            status, data, dt3, _ = http_get(url, args.timeout, args.proxy)
            sums = parse_sums(data.decode("utf-8", "replace"))
            if not sums:
                results_note = f"HTTP {status} 但解析不出条目"
                print(f"    {label:<32}: FAIL {results_note}")
                continue
            sums_by_src[label] = sums
            print(f"    {label:<32}: OK   {len(sums)} 条 ({dt3*1000:.0f}ms, {len(data)}B)")
        except Exception as e:
            print(f"    {label:<32}: FAIL {type(e).__name__}: {e}"[:80])

    ref = None
    if sums_by_src:
        values = list(sums_by_src.values())
        ref = values[0]
        consistent = all(v == ref for v in values[1:])
        print(f"    一致性：{'PASS —— 各来源逐条一致' if consistent else 'FAIL —— 来源间哈希不一致（有镜像在篡改或损坏内容）！'}")
        if not consistent:
            for name, sums in sums_by_src.items():
                for k in ref:
                    if sums.get(k) != ref[k]:
                        print(f"      差异：{name} 的 {k}")
            print("    ⇒ 存在不一致来源时，任何镜像都不应配置进 update.mirror。")

    # ---- 5) 逐镜像下载试速 + 完整性 ----
    print(f"\n[5] 下载检测：经镜像拉取 {pick}"
          f"（{'全量' if args.full else f'Range 前 {args.range_bytes//1024}KB'}）")
    full_small = args.full or (pick in sizes and sizes[pick] < 3 * 1024 * 1024)
    probe_assets = ["SHA256SUMS.txt"] if "SHA256SUMS.txt" in names else []
    if pick not in probe_assets:
        probe_assets.append(pick)

    for m in mirrors:
        gates = []
        dl_ok_all = True
        for aname in probe_assets:
            url = asset_url(m, assets[aname])
            try:
                want_full = full_small or aname == "SHA256SUMS.txt"
                if want_full:
                    status, data, dt4, _ = http_get(url, args.timeout, args.proxy)
                    got = len(data)
                    extra = ""
                    if ref and aname in ref:
                        h = hashlib.sha256(data).hexdigest()
                        match = h == ref[aname]
                        extra = f"  sha256 {'PASS' if match else 'MISMATCH!'}"
                        if not match:
                            dl_ok_all = False
                    else:
                        extra = "  (无基准哈希，仅验可达)"
                else:
                    status, data, dt4, _ = http_get(
                        url, args.timeout, args.proxy,
                        headers={"Range": f"bytes=0-{args.range_bytes - 1}"})
                    got = len(data)
                    extra = "  (Range 试速，不验哈希)"
                gates.append((f"下载 {aname}", True,
                              f"{got}B / {dt4:.2f}s = {speed_note(got, dt4)}{extra}"))
                if aname != "SHA256SUMS.txt" and got < 1024:
                    dl_ok_all = False
                    gates[-1] = (gates[-1][0], False, f"仅回 {got}B，疑似错误页")
            except urllib.error.HTTPError as e:
                dl_ok_all = False
                gates.append((f"下载 {aname}", False, f"HTTP {e.code}"))
            except Exception as e:
                dl_ok_all = False
                gates.append((f"下载 {aname}", False, f"{type(e).__name__}: {e}"[:70]))
        verdict = "PASS" if dl_ok_all else "FAIL"
        results.append((m, verdict, gates))

    # 直连基线
    if args.proxy is None:
        gates = []
        try:
            url = assets[pick]
            status, data, dt5, _ = http_get(
                url, args.timeout, None,
                headers={"Range": f"bytes=0-{args.range_bytes - 1}"})
            gates.append((f"下载 {pick}", True, f"{len(data)}B / {dt5:.2f}s = {speed_note(len(data), dt5)} (Range)"))
            results.append(("直连(基线)", "PASS" if len(data) >= 1024 else "FAIL", gates))
        except Exception as e:
            results.append(("直连(基线)", "FAIL", [(f"下载 {pick}", False, f"{type(e).__name__}: {e}"[:70])]))

    # ---- 汇总 ----
    print("\n" + "=" * 72)
    print(" 汇总")
    print("=" * 72)
    any_pass = False
    for name, verdict, gates in results:
        print(f"\n  [{verdict}] {name}")
        for g, okg, note3 in gates:
            print(f"      {'✔' if okg else '✘'} {g:<28} {note3}")
        if verdict == "PASS" and name != "直连(基线)":
            any_pass = True
        if name != "直连(基线)" and api_via.get(name):
            print(f"      ✔ API 转发额外可用（update.mirror 也可兼作检查源）")
    print("\n结论：" + ("至少一个镜像通过全部门槛 → 可作为 update.mirror 候选（详见 AUTO_UPDATE_PLAN.md §3.4："
                      "镜像只由用户显式配置，产品永不内置默认值）。"
                      if any_pass else "无可用镜像 → 仅直连/本地代理路径，或更换候选清单（--mirror）。"))
    return 0 if any_pass else 1


def _asset_sizes(api_url, timeout, proxy):
    try:
        _, data, _, _ = http_get(api_url, timeout, proxy)
        rel = json.loads(data)
        return {a["name"]: a["size"] for a in rel.get("assets", [])}
    except Exception:
        return {}


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    sys.exit(main())
