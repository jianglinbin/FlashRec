#!/usr/bin/env python3
"""SOAP 收件箱探针（d144）。

用途：验证「请求原文收件箱」（third_party/pupnp 构建期补丁 fr_soap_inbox →
logs/soap_inbox.log）确实在**入口**把请求原文逐字节落盘，并顺带演示
控制点「先发 url、再发进度」这一类二次推送长什么样。

三个请求：
  1. GetMediaInfo                     —— 只读，无副作用，用来确认收件箱在工作
  2. SetAVTransportURI <本地文件>      —— 首次投屏（等价于控制点第一步）
  3. SetAVTransportURI <同一 URI>#t=7 —— 同 URI 二次推送（等价于"再发进度信息"）
     ★ 这一条会命中 player_controller::handle_set_uri 的 DuplicateURI 分支：
       若 URI 带 #t= 片段 → 转成 Seek（符合预期）；
       若 URI 完全一致且无片段 → 只打 WARN「DuplicateURI ignored」并 return。
       两条路径的内容都**应当**完整出现在 soap_inbox.log 里（它在 dispatch_request
       第一条语句上落盘，早于任何内部逻辑）。

用法：
    python tests/soap_inbox_probe.py                 # 用默认本地 mp4
    python tests/soap_inbox_probe.py --uri <URI>     # 换成任意 URI
    python tests/soap_inbox_probe.py --host 192.0.2.1
仅标准库，无需第三方依赖。
"""

import argparse
import urllib.error
import urllib.request
import time

SVC = "urn:schemas-upnp-org:service:AVTransport:1"
DEFAULT_URI = "file:///D:/dev/FlashRec/tools/archive/c_mp4v.mp4"

DIDL_TPL = (
    '<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/"'
    ' xmlns:dc="http://purl.org/dc/elements/1.1/"'
    ' xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/">'
    '<item id="0" parentID="-1" restricted="1">'
    "<dc:title>INBOX-PROBE</dc:title>"
    "<upnp:class>object.item.videoItem</upnp:class>"
    '<res protocolInfo="http-get:*:video/mp4:DLNA.ORG_OP=01;DLNA.ORG_CI=0">{}</res>'
    "</item></DIDL-Lite>"
)


def call(endpoint, action, body, tag):
    """发一条 SOAP action；SOAP fault 也算正常返回（说明协议层应答了）。"""
    env = (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"'
        ' s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
        "<u:" + action + ' xmlns:u="' + SVC + '">' + body + "</u:" + action + ">"
        "</s:Body></s:Envelope>"
    )
    req = urllib.request.Request(
        endpoint,
        data=env.encode("utf-8"),
        method="POST",
        headers={
            "Content-Type": 'text/xml; charset="utf-8"',
            "SOAPACTION": '"' + SVC + "#" + action + '"',
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=8) as r:
            print("[%s] %-18s HTTP %s" % (tag, action, r.status))
            return True
    except urllib.error.HTTPError as e:
        print("[%s] %-18s HTTP %s（SOAP fault）" % (tag, action, e.code))
        return True
    except Exception as e:  # noqa: BLE001 - 探针，任何异常都要看得见
        print("[%s] %-18s 连接失败：%s" % (tag, action, e))
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1", help="接收端地址")
    ap.add_argument("--port", type=int, default=49152)
    ap.add_argument("--uri", default=DEFAULT_URI, help="投屏 URI")
    ap.add_argument("--gap", type=float, default=3.0, help="首次投屏到二次推送的间隔秒")
    a = ap.parse_args()

    ep = "http://%s:%d/fr/control/AVTransport" % (a.host, a.port)
    didl = DIDL_TPL.format(a.uri)
    print("目标 %s\nURI  %s\n" % (ep, a.uri))

    if not call(ep, "GetMediaInfo", "<InstanceID>0</InstanceID>", "1 只读"):
        raise SystemExit("接收端不可达")
    call(
        ep,
        "SetAVTransportURI",
        "<InstanceID>0</InstanceID><CurrentURI>%s</CurrentURI>"
        "<CurrentURIMetaData>%s</CurrentURIMetaData>" % (a.uri, didl),
        "2 首次投屏",
    )
    time.sleep(a.gap)
    call(
        ep,
        "SetAVTransportURI",
        "<InstanceID>0</InstanceID><CurrentURI>%s#t=7</CurrentURI>"
        "<CurrentURIMetaData>%s</CurrentURIMetaData>" % (a.uri, didl),
        "3 二次推送 #t=7",
    )
    print("\n完成。核对：logs/soap_inbox.log 里应出现 3 段原文，")
    print("其中第 3 段的 CurrentURI 必须带 '#t=7'（它是逐字节落盘的）。")


if __name__ == "__main__":
    main()
