"""下载 mpv-dev（libmpv 预编译开发包）并解压到 third_party/mpv/。

★ 版本钉死（2026-09-17 起）：**不再取滚动 latest**。
   zhongfly/mpv-winbuild 每天重发，取 latest 会在无人察觉的情况下换掉 libmpv
   （2026-09-15 那次就无声把 libmpv 换成 mpv 0.41.0），出问题后既无法复现也无法回退。
   现在钉死 release tag + 资产名；想升级就改下面两个常量（或临时用环境变量覆盖）：
     FR_MPV_TAG=2026-09-16-0b7ed670f7 FR_MPV_ASSET=... python tools/fetch_mpv.py
   并用 tests/ + %TEMP%\flashrec_diag\pix_probe.py 的像素回读验收新版本真能出画面。

7z 用系统自带 bsdtar（C:/Windows/system32/tar.exe）解压。
"""

from __future__ import annotations

import os
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

# CI/Windows 控制台默认非 UTF-8，打印中文会 UnicodeEncodeError（实测 CI 踩过）→ 强制 UTF-8。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
except Exception:
    pass

ROOT = Path(__file__).resolve().parent.parent
DEST = ROOT / "third_party" / "mpv"
TAR = r"C:\Windows\system32\tar.exe"

# —— 钉住的版本（唯一真值；改这里等于升级 libmpv）——
TAG = "2026-09-14-0b7ed670f7"
ASSET = "mpv-dev-x86_64-20260914-git-0b7ed670f7.7z"

URL_TMPL = "https://github.com/zhongfly/mpv-winbuild/releases/download/{tag}/{asset}"


def main() -> int:
    tag = os.environ.get("FR_MPV_TAG", TAG)
    asset = os.environ.get("FR_MPV_ASSET", ASSET)
    url = URL_TMPL.format(tag=tag, asset=asset)
    stamp = f"{tag}/{asset}"

    # 已就绪且**版本戳一致**才跳过 —— 只查 .done 是否存在会让"改了钉版本但目录还在"
    # 的情况静默跳过，正是老代码的坑。
    done = DEST / ".done"
    if done.is_file():
        cur = done.read_text(encoding="utf-8").strip()
        if cur == stamp:
            print(f"[skip] third_party/mpv 已就绪且版本戳一致: {stamp}")
            return 0
        print(f"[stale] 版本戳不一致，重新下载\n        现有: {cur}\n        期望: {stamp}")

    DEST.mkdir(parents=True, exist_ok=True)
    print(f"[pin] mpv-dev = {stamp}")
    arch = DEST / asset

    req = urllib.request.Request(url, headers={"User-Agent": "flashrec-deps"})
    try:
        with urllib.request.urlopen(req, timeout=600) as r:
            total = int(r.headers.get("Content-Length") or 0)
            print(f"[dl] {asset} ({total / 1e6:.1f} MB)" if total else f"[dl] {asset}")
            got = 0
            with open(arch, "wb") as f:
                while True:
                    chunk = r.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
                    got += len(chunk)
    except urllib.error.HTTPError as e:
        print(f"[err] 下载失败 {e.code}: {url}\n"
              f"      钉住的 tag/资产可能已被上游清理，请改 TAG/ASSET 常量或设 FR_MPV_TAG/FR_MPV_ASSET")
        return 1
    if got == 0:
        print("[err] 下载到 0 字节")
        return 1

    print("[extract] ->", DEST)
    subprocess.run([TAR, "-xf", str(arch), "-C", str(DEST)], check=True)
    done.write_text(stamp, encoding="utf-8")
    arch.unlink(missing_ok=True)
    print("[ok] mpv-dev 就绪")
    return 0


if __name__ == "__main__":
    sys.exit(main())
