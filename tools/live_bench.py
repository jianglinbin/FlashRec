#!/usr/bin/env python3
"""起播延迟离线台（d285）：无手机条件下复现并度量 AirPlay 回流的「A 段」。

为什么需要它：起播门只在**生产 player**（libmpv render + GL FBO + per-file 低延迟选项）
上出现；离线裸 libmpv（vo=null,ao=null）实测 A=0.01s，与真机 1.6~7.7s 差两三个数量级
（判据见台账 d283）。本台用 `tcp://127.0.0.1:PORT` 把切片按实时速率喂给正在运行的
flashrec，经 DLNA SetAVTransportURI 加载（需 App 侧 `FR_LIVE_FORMAT=mpegts` 才按
回流实时流对待），再从日志里读 A/B 段与稳态积压。

用法:
  python tools/live_bench.py <ts文件> [速率MB/s=0.29] [观察秒=12] [端口=17199]
前置:
  1) flashrec 正在运行，且环境变量 FR_LIVE_FORMAT=mpegts（+ 可选 FR_MPV_TRACE=1）
  2) 换选项组合后要**重启 App**（FR_LIVE_OPTS 是进程级环境变量）
输出: A / B / 稳态接收端积压（三个数就是起播延迟的全部构成）
"""
import re
import subprocess
import sys
import threading
import time
import urllib.request

HOST = "http://127.0.0.1:49152"
SERVICE = "urn:schemas-upnp-org:service:AVTransport:1"
LOG = r"C:\Users\ben\AppData\Roaming\FlashRec\logs\flashrec.log"


def soap(action: str, body: str = "") -> None:
    env = (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
        's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
        f'<s:Body><u:{action} xmlns:u="{SERVICE}">{body}</u:{action}>'
        "</s:Body></s:Envelope>"
    )
    req = urllib.request.Request(
        f"{HOST}/fr/control/AVTransport", data=env.encode("utf-8"), method="POST",
        headers={"Content-Type": 'text/xml; charset="utf-8"', 'SOAPAction': f'"{SERVICE}#{action}"'},
    )
    with urllib.request.urlopen(req, timeout=5) as r:
        r.read()


def set_uri(uri: str) -> None:
    didl = ("&lt;DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\"&gt;"
            "&lt;item id=\"1\" parentID=\"0\" restricted=\"0\"&gt;"
            "&lt;dc:title&gt;bench&lt;/dc:title&gt;"
            f"&lt;res protocolInfo=\"http-get:*:video/mp2t:*\"&gt;{uri}&lt;/res&gt;"
            "&lt;/item&gt;&lt;/DIDL-Lite&gt;")
    soap("SetAVTransportURI",
         f"<InstanceID>0</InstanceID><CurrentURI>{uri}</CurrentURI>"
         f"<CurrentURIMetaData>{didl}</CurrentURIMetaData>")


def scan(patterns: dict[str, re.Pattern], deadline: float, found: dict) -> None:
    """A/B 取**首次**命中（起播是一次性事件），steady 取**末次**命中（要读收敛后的积压）。"""
    with open(LOG, "r", encoding="utf-8", errors="replace") as fp:
        fp.seek(0, 2)
        while time.time() < deadline:
            line = fp.readline()
            if not line:
                time.sleep(0.02)
                continue
            for key, rx in patterns.items():
                if rx.search(line) and (key == "steady" or key not in found):
                    found[key] = line.rstrip()


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    rate = float(sys.argv[2]) if len(sys.argv) > 2 else 0.29
    watch = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0
    port = int(sys.argv[4]) if len(sys.argv) > 4 else 17199

    feed = subprocess.Popen(
        [sys.executable, "build/test_ts/ts_feed.py", path, str(port), str(rate), str(watch + 30)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.8)
    found: dict = {}
    pats = {
        "A": re.compile(r"起播取证 A 段: loadfile→file-loaded=([\d.]+)s"),
        "B": re.compile(r"起播取证 B 段: file-loaded→首个 time-pos=([\d.]+)s"),
        "steady": re.compile(r"延迟探针: 流经过=[\d.]+s time-pos=[\d.-]+ 接收端积压=([\d.]+)s.*加载=yes"),
    }
    t = threading.Thread(target=scan, args=(pats, time.time() + watch + 6, found), daemon=True)
    t.start()
    uri = f"tcp://127.0.0.1:{port}"
    set_uri(uri)
    soap("Play", "<InstanceID>0</InstanceID>")
    print(f"#{uri} rate={rate}MB/s watch={watch}s")
    time.sleep(watch)
    try:
        soap("Stop", "<InstanceID>0</InstanceID>")
    except Exception:
        pass
    feed.terminate()
    for key in ("A", "B", "steady"):
        if key in found:
            m = pats[key].search(found[key])
            print(f"{key:7s} = {m.group(1)} s")
        else:
            print(f"{key:7s} = （未出现）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
