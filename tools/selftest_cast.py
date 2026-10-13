#!/usr/bin/env python3
"""FlashRec 自测投屏脚本：向本机 DMR 发送 SetAVTransportURI + Play。

用法:
  python tools/selftest_cast.py [URL]        # 默认投 BigBuckBunny 测试流
  python tools/selftest_cast.py stop         # 发 Stop
"""
import sys
import urllib.request

HOSTS = ("http://127.0.0.1:49152", "http://192.0.2.1:49152")
SERVICE = "urn:schemas-upnp-org:service:AVTransport:1"
DEFAULT_URL = ("http://commondatastorage.googleapis.com/gtv-videos-bucket/"
               "sample/BigBuckBunny.mp4")
DIDL = (
    '&lt;DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" '
    'xmlns:dc="http://purl.org/dc/elements/1.1/" '
    'xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/"&gt;'
    '&lt;item id="1" parentID="0" restricted="0"&gt;'
    '&lt;dc:title&gt;BigBuckBunny 自测流&lt;/dc:title&gt;'
    '&lt;upnp:class&gt;object.item.videoItem&lt;/upnp:class&gt;'
    '&lt;res protocolInfo="http-get:*:video/mp4:*"&gt;' + DEFAULT_URL +
    '&lt;/res&gt;&lt;/item&gt;&lt;/DIDL-Lite&gt;'
)


def soap(action: str, body: str) -> None:
    env = (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
        's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
        f'<s:Body><u:{action} xmlns:u="{SERVICE}">{body}</u:{action}>'
        '</s:Body></s:Envelope>'
    )
    last: Exception | None = None
    for base in HOSTS:
        req = urllib.request.Request(
            f"{base}/fr/control/AVTransport", data=env.encode("utf-8"), method="POST",
            headers={
                "Content-Type": 'text/xml; charset="utf-8"',
                "SOAPACTION": f'"{SERVICE}#{action}"',
            })
        try:
            with urllib.request.urlopen(req, timeout=5) as resp:
                print(f"{action} @ {base}: HTTP {resp.status}")
                return
        except Exception as e:  # noqa: BLE001
            last = e
    print(f"{action}: FAILED {last}")
    sys.exit(1)


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1] == "stop":
        soap("Stop", "<InstanceID>0</InstanceID>")
        return
    url = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_URL
    # ★URL 含 & 时必须 XML 转义（d151）：真实客户端会转义，手工拼报文忘了就会被
    # 服务端判非法 XML → HTTP 400（bdstatic 等带查询串的源全中招）。
    url_x = url.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    didl = DIDL if url == DEFAULT_URL else DIDL.replace(DEFAULT_URL, url_x)
    soap("SetAVTransportURI",
         f"<InstanceID>0</InstanceID><CurrentURI>{url_x}</CurrentURI>"
         f"<CurrentURIMetaData>{didl}</CurrentURIMetaData>")
    soap("Play", "<InstanceID>0</InstanceID><Speed>1</Speed>")


if __name__ == "__main__":
    main()
