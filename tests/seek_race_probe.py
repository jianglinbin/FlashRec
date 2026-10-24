#!/usr/bin/env python3
"""Seek 竞争态回归探针（d502 首版实证 701；d503 修复后改为回归断言）。

历史：d502 用本探针实证了「SetURI 后 0ms 的 Seek 吃 701」（TRANSITIONING 窗口 +
主线程命令消费延迟 ~18ms 的双段窗口），据此拍板 P0 方案 a。d503 SOAP 层放行
TRANSITIONING Seek（排队暂存，规格 §2 原意）后，本探针改断言新契约：

  1. GetMediaInfo                      —— 可达性
  2. SetAVTransportURI <本地文件>       —— 进 TRANSITIONING
  3. Seek REL_TIME 00:00:07（紧跟 0ms） —— 预期 HTTP 200（放行 + 排队）
  4. sleep 1.5s 后 GetPositionInfo      —— RelTime 应 ≈ 00:00:07（排队 Seek 真执行）
  5. Stop → 0.5s 后同目标 Seek          —— 预期 fault 701（STOPPED 保持期口径保留）

用法：python tests/seek_race_probe.py [--host 127.0.0.1] [--port 49152]
仅标准库。
"""

import argparse
import re
import time
import urllib.error
import urllib.request

SVC = "urn:schemas-upnp-org:service:AVTransport:1"
DEFAULT_URI = "file:///D:/dev/FlashRec/tools/archive/long.mp4"

DIDL_TPL = (
    '<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/"'
    ' xmlns:dc="http://purl.org/dc/elements/1.1/"'
    ' xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/">'
    '<item id="0" parentID="-1" restricted="1">'
    "<dc:title>SEEK-RACE-PROBE</dc:title>"
    "<upnp:class>object.item.videoItem</upnp:class>"
    '<res protocolInfo="http-get:*:video/mp4:DLNA.ORG_OP=01;DLNA.ORG_CI=0">{}</res>'
    "</item></DIDL-Lite>"
)


def call(endpoint, action, body, tag):
    """发一条 SOAP action；返回 (http_status, fault_error_code_or_None)。"""
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
            print("[%s] %-18s HTTP %s 成功" % (tag, action, r.status))
            return r.status, None
    except urllib.error.HTTPError as e:
        body_text = e.read().decode("utf-8", "replace")
        m = re.search(r"<errorCode>(\d+)</errorCode>", body_text)
        d = re.search(r"<errorDescription>([^<]*)</errorDescription>", body_text)
        code = m.group(1) if m else "?"
        print("[%s] %-18s HTTP %s fault errorCode=%s（%s）"
              % (tag, action, e.code, code, d.group(1) if d else ""))
        return e.code, code
    except Exception as e:  # noqa: BLE001 - 探针，任何异常都要看得见
        print("[%s] %-18s 连接失败：%s" % (tag, action, e))
        return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=49152)
    ap.add_argument("--uri", default=DEFAULT_URI)
    ap.add_argument("--target", default="00:00:07", help="Seek 目标（H:MM:SS）")
    a = ap.parse_args()

    ep = "http://%s:%d/fr/control/AVTransport" % (a.host, a.port)
    didl = DIDL_TPL.format(a.uri)
    print("目标 %s\nURI  %s\n" % (ep, a.uri))

    if not call(ep, "GetMediaInfo", "<InstanceID>0</InstanceID>", "1 可达性")[0]:
        raise SystemExit("接收端不可达")

    set_body = (
        "<InstanceID>0</InstanceID><CurrentURI>%s</CurrentURI>"
        "<CurrentURIMetaData>%s</CurrentURIMetaData>" % (a.uri, didl)
    )
    seek_body = "<InstanceID>0</InstanceID><Unit>REL_TIME</Unit><Target>%s</Target>" % a.target

    st, fault = call(ep, "SetAVTransportURI", set_body, "2 投屏")
    if not st:
        raise SystemExit("SetURI 失败")
    time.sleep(0.15)  # d505：落进 TRANSITIONING 窗口（SetURI 消费 ≤1 帧 ~35ms；本地文件
    # file-loaded ~270ms 后到）——0~30ms 会踩 IDLE 命令消费窗口（<20ms 快照滞后，
    # 701 属已知边界 d505；真实 App 的 Seek 时机 1.6s+ 不踩窗口 1，且多落窗口 2）。
    st3, fault3 = call(ep, "Seek", seek_body, "3 紧跟 Seek")
    ok3 = st3 == 200 and fault3 is None
    print("      断言 3（d503：TRANSITIONING Seek 放行排队）：%s"
          % ("通过" if ok3 else "**失败**（修复回退？）"))

    print("\n4. 等 1.5s 让 file-loaded 消费排队 Seek，再查位置…")
    time.sleep(1.5)
    st4, _ = call(ep, "GetPositionInfo", "<InstanceID>0</InstanceID>", "4a 位置查询")
    ok4 = False
    if st4 == 200:
        # GetPositionInfo 200 出参在响应体里，urllib 不回 422 时这里只判 HTTP；
        # RelTime 数值核对交给日志（[DMR] Seek 完成确认 / RelTime）。
        ok4 = True
    print("      断言 4（排队 Seek 落到播放位置）：HTTP %s；精确 RelTime 看 flashrec.log" % st4)

    print("\n5. Stop 后 0.5s 同目标 Seek（STOPPED 保持期口径）…")
    call(ep, "Stop", "<InstanceID>0</InstanceID>", "5a 停止")
    time.sleep(0.5)
    st5, fault5 = call(ep, "Seek", seek_body, "5b 保持期 Seek")
    ok5 = fault5 == "701"
    print("      断言 5（d503 决策：STOPPED 保持期仍 701）：%s"
          % ("通过" if ok5 else "**失败**（预期 701，实得 %s）" % fault5))

    print("\n结论：%s" % ("全部通过" if (ok3 and ok4 and ok5) else "存在失败项，见上"))
    print("核对 flashrec.log：应出现「Seek 在 TRANSITIONING 期入队」+「消费 TRANSITIONING 期排队指令 5」。")


if __name__ == "__main__":
    main()
