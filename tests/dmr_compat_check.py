#!/usr/bin/env python3
"""DMR 协议兼容性断言（只发查询型动作，不改任何设备状态）。

用一条命令守住四类"值对了但控制点拿不到"的回归——这四类都是 2026-09-17 实测
踩出来的（bilibili 投屏后手机 UI 零反馈的根因链）：

  1. **响应元素必须在服务类型命名空间内**（`<u:XxxResponse xmlns:u="服务类型">`）。
     只写 xmlns:u 而元素不带前缀 ⇒ 元素落空命名空间；只比标签名的解析器照常命中，
     但 namespace-aware 解析器（Go encoding/xml、.NET、JAXB、lxml）必然取不到。
  2. **出参名必须与 SCPD 声明逐字一致**（差一个 Current 前缀 = 控制点取不到值）。
  3. **协议侧时间串必须是 HH:MM:SS**（补零；`0:03:49` 会被 iOS 的 HH:mm:ss 严格解析拒掉）。
  4. **错误码必须原样透出**：libupnp 在 ErrStr 为空时会把一切失败改写成 501
     ⇒ 401/402/701/710 全退化，控制点无法区分"不支持/参数错/状态不允许/无此项"。

外加两条静态一致性：**SCPD 与实现的动作集合双向相等**、**evented 变量清单与实现一致**。

用法：
  python tests/dmr_compat_check.py                 # 自动找 127.0.0.1 / 192.0.2.1
  python tests/dmr_compat_check.py --host 192.0.2.10:49152
退出码 0 = 全绿。
"""
from __future__ import annotations

import argparse
import re
import socket
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCPD_DIR = ROOT / "assets" / "scpd"

AVT = "urn:schemas-upnp-org:service:AVTransport:1"
RCS = "urn:schemas-upnp-org:service:RenderingControl:1"
CM = "urn:schemas-upnp-org:service:ConnectionManager:1"
SOAP_ENV = "http://schemas.xmlsoap.org/soap/envelope/"

SERVICES = {
    "AVT": (AVT, "/fr/control/AVTransport", "AVTransport.xml"),
    "RCS": (RCS, "/fr/control/RenderingControl", "RenderingControl.xml"),
    "CM": (CM, "/fr/control/ConnectionManager", "ConnectionManager.xml"),
}

# 实现里确实存在的动作集合（改动 soap_util.cpp 的动作分支时同步这里；
# 本脚本会用它与 SCPD 做双向比对，任一侧漂移都会红）。
IMPLEMENTED = {
    "AVT": {
        "SetAVTransportURI", "SetNextAVTransportURI", "GetMediaInfo", "GetTransportInfo",
        "GetPositionInfo", "GetDeviceCapabilities", "GetTransportSettings",
        "GetCurrentTransportActions", "Stop", "Play", "Pause", "Seek", "Next", "Previous",
    },
    "RCS": {
        "ListPresets", "SelectPreset", "GetVolume", "SetVolume", "GetMute", "SetMute",
        "GetVolumeDB", "SetVolumeDB", "GetVolumeDBRange",
        *{f"Get{n}" for n in ("Brightness", "Contrast", "Sharpness", "ColorTemperature",
                              "RedVideoGain", "GreenVideoGain", "BlueVideoGain",
                              "HorizontalKeystone", "VerticalKeystone")},
        *{f"Set{n}" for n in ("Brightness", "Contrast", "Sharpness", "ColorTemperature",
                              "RedVideoGain", "GreenVideoGain", "BlueVideoGain",
                              "HorizontalKeystone", "VerticalKeystone")},
    },
    "CM": {"GetProtocolInfo", "GetCurrentConnectionIDs", "GetCurrentConnectionInfo"},
}

# CM:1 的事件模型不是 LastChange，而是这三个变量各自 evented
CM_EVENTED = {"SourceProtocolInfo", "SinkProtocolInfo", "CurrentConnectionIDs"}

HMS = re.compile(r"^\d{2}:\d{2}:\d{2}$")

_fail: list[str] = []
_pass = 0


def check(ok: bool, label: str, detail: str = "") -> None:
    global _pass
    if ok:
        _pass += 1
    else:
        _fail.append(f"{label}{('  ← ' + detail) if detail else ''}")
    print(f"  [{'OK ' if ok else 'FAIL'}] {label}" + (f"  {detail}" if detail and not ok else ""))


def soap(host: str, path: str, svc: str, action: str, inner: str) -> tuple[int, str]:
    env = (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
        's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
        f'<s:Body><u:{action} xmlns:u="{svc}">{inner}</u:{action}></s:Body></s:Envelope>'
    ).encode("utf-8")
    head = (
        f"POST {path} HTTP/1.1\r\nHost: {host}\r\n"
        'Content-Type: text/xml; charset="utf-8"\r\n'
        f'SOAPACTION: "{svc}#{action}"\r\n'
        f"Content-Length: {len(env)}\r\nConnection: close\r\n\r\n"
    ).encode("ascii")
    ip, port = host.rsplit(":", 1)
    s = socket.create_connection((ip, int(port)), timeout=6)
    s.sendall(head + env)
    buf = b""
    while True:
        b = s.recv(65536)
        if not b:
            break
        buf += b
    s.close()
    text = buf.decode("utf-8", "replace")
    m = re.match(r"HTTP/1\.\d (\d+)", text)
    body = text.split("\r\n\r\n", 1)[1] if "\r\n\r\n" in text else ""
    return int(m.group(1)) if m else 0, body


def parse_scpd(name: str) -> tuple[set[str], set[str], dict[str, list[str]]]:
    """返回 (动作名集合, evented 变量集合, {动作: [out 参数名...]})。

    SCPD 的根元素带默认命名空间（`urn:schemas-upnp-org:service-1-0`），
    `ElementTree.iter("action")` 是**带命名空间**匹配、会一个都找不到 ⇒ 一律按本地名比。
    """
    tree = ET.parse(SCPD_DIR / name)
    root = tree.getroot()

    def lname(el: ET.Element) -> str:
        return el.tag.rsplit("}", 1)[-1]

    def child_text(el: ET.Element, want: str) -> str:
        for ch in el:  # 只看直接子元素（避免把 argument 里的 name 当动作名）
            if lname(ch) == want:
                return (ch.text or "").strip()
        return ""

    actions: set[str] = set()
    out_args: dict[str, list[str]] = {}
    for act in root.iter():
        if lname(act) != "action":
            continue
        nm = child_text(act, "name")
        actions.add(nm)
        outs: list[str] = []
        for a in act.iter():
            if lname(a) != "argument":
                continue
            if child_text(a, "direction") == "out":
                outs.append(child_text(a, "name"))
        out_args[nm] = outs

    evented: set[str] = set()
    for sv in root.iter():
        if lname(sv) == "stateVariable" and sv.get("sendEvents") == "yes":
            evented.add(child_text(sv, "name"))
    return actions, evented, out_args


def q(svc: str, action: str, body: str) -> ET.Element | None:
    """命名空间感知地取出响应元素；取不到返回 None。"""
    try:
        root = ET.fromstring(body)
    except ET.ParseError:
        return None
    return root.find(f"{{{SOAP_ENV}}}Body/{{{svc}}}{action}Response")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="", help="ip:port；缺省自动探测")
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
        print("找不到可用的设备端点（用 --host ip:port 指定）")
        return 2
    print(f"### 目标设备 {host}\n")

    # ---- A. SCPD 与实现双向一致 + evented 清单 ----
    print("A. SCPD ↔ 实现 一致性")
    out_arg_map: dict[str, list[str]] = {}
    for key, (_, _, scpd_name) in SERVICES.items():
        declared, evented, out_args = parse_scpd(scpd_name)
        out_arg_map.update(out_args)
        check(declared == IMPLEMENTED[key], f"{key} SCPD 动作集 == 实现动作集",
              f"仅SCPD有={sorted(declared - IMPLEMENTED[key])} 仅实现有={sorted(IMPLEMENTED[key] - declared)}")
        want = CM_EVENTED if key == "CM" else {"LastChange"}
        check(evented == want, f"{key} evented 变量清单 == {sorted(want)}",
              f"实际={sorted(evented)}")

    # ---- B. 响应命名空间（根因项）----
    print("\nB. 响应元素命名空间（namespace-aware 必须命中）")
    probes = [
        ("AVT", "GetTransportInfo", "<InstanceID>0</InstanceID>"),
        ("AVT", "GetPositionInfo", "<InstanceID>0</InstanceID>"),
        ("AVT", "GetMediaInfo", "<InstanceID>0</InstanceID>"),
        ("AVT", "GetCurrentTransportActions", "<InstanceID>0</InstanceID>"),
        ("RCS", "GetVolume", "<InstanceID>0</InstanceID><Channel>Master</Channel>"),
        ("RCS", "ListPresets", "<InstanceID>0</InstanceID>"),
        ("CM", "GetProtocolInfo", ""),
    ]
    seen: dict[str, ET.Element] = {}
    for key, action, inner in probes:
        svc, path, _ = SERVICES[key]
        status, body = soap(host, path, svc, action, inner)
        node = q(svc, action, body)
        seen[f"{key}/{action}"] = node
        tag = node.tag.split("}")[1] if node is not None else "-"
        check(status == 200 and node is not None,
              f"{action} 响应元素 {{服务类型}}{action}Response 可被命名空间感知解析",
              f"HTTP {status} 首元素={tag}")

    # ---- C. 出参名与 SCPD 一致 ----
    print("\nC. 出参名 == SCPD 声明")
    for key, action in (("AVT", "GetTransportInfo"), ("AVT", "GetPositionInfo"),
                        ("AVT", "GetCurrentTransportActions"), ("RCS", "GetVolume"),
                        ("RCS", "ListPresets"), ("CM", "GetProtocolInfo")):
        node = seen[f"{key}/{action}"]
        if node is None:
            check(False, f"{action} 出参名", "响应不可解析")
            continue
        got = [c.tag for c in node]  # 出参元素应**不带命名空间前缀**
        want = out_arg_map.get(action, [])
        check(got == want, f"{action} 出参名与顺序 == SCPD", f"实际={got} 期望={want}")

    # ---- D. 协议时间串 HH:MM:SS ----
    print("\nD. 协议时间串格式")
    node = seen["AVT/GetPositionInfo"]
    if node is not None:
        for f in ("TrackDuration", "RelTime", "AbsTime"):
            v = (node.findtext(f) or "")
            check(bool(HMS.match(v)), f"GetPositionInfo/{f} 形如 HH:MM:SS", f"实际={v!r}")

    # ---- E. 错误码原样透出 ----
    print("\nE. 错误码透出（libupnp ErrStr 陷阱）")
    svc, path, _ = SERVICES["AVT"]
    status, body = soap(host, path, svc, "GetFooBarNotExist", "<InstanceID>0</InstanceID>")
    code = re.search(r"<errorCode>(\d+)</errorCode>", body)
    check(code is not None and code.group(1) == "401",
          "未知动作 → 401 Invalid Action", f"HTTP {status} code={code.group(1) if code else '?'}")
    status, body = soap(host, path, svc, "GetPositionInfo", "")
    code = re.search(r"<errorCode>(\d+)</errorCode>", body)
    check(code is not None and code.group(1) == "402",
          "缺必填参数 → 402 Invalid Args", f"HTTP {status} code={code.group(1) if code else '?'}")
    status, body = soap(host, path, svc, "Previous", "<InstanceID>0</InstanceID>")
    code = re.search(r"<errorCode>(\d+)</errorCode>", body)
    check(code is not None and code.group(1) == "710",
          "Previous 无上一项 → 710 No such item", f"HTTP {status} code={code.group(1) if code else '?'}")

    # ---- F. 预置名规范拼写 ----
    print("\nF. PresetNameList 规范拼写")
    node = seen["RCS/ListPresets"]
    v = (node.findtext("CurrentPresetNameList") or "") if node is not None else ""
    check(v == "FactoryDefaults", "ListPresets.CurrentPresetNameList == FactoryDefaults", f"实际={v!r}")

    print(f"\n===== 通过 {_pass} / 失败 {len(_fail)} =====")
    for f in _fail:
        print(f"  ✗ {f}")
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
