#!/usr/bin/env python3
"""github_publish.py —— 项目推送 GitHub 一键化（project-doc-rules 提供）。

子命令：
  token set [--stdin]     保存 token（env > keyring加密 > 明文文件，见 token 政策）
  token check             校验 token（GET /user，不回显）
  repo create NAME [--private] [--desc D]   建仓（已存在则直接采用）
  push [--tags] [--allow-dirty] [--branch B] 推送当前分支（+tag），token 不入 git config
  ci trigger [--workflow W] [--ref R]       触发 workflow_dispatch（无则提示走 tag 触发）
  ci status [--watch] [--workflow W]        查询/等待最近一次 CI 运行结论
  publish [--private] [--tags] [--watch]    全流程：建仓(缺才建) → push → CI 触发+等待

token 政策（技能文档已授权，本脚本据此实现）：
  读取优先级：环境变量 GITHUB_TOKEN / GH_TOKEN → keyring 加密存储（需 pip install keyring，
  落 Windows 凭据管理器 / macOS 钥匙串 / Linux Secret Service）→ 明文文件（权限 0600；
  Windows 依赖用户Profile ACL）。明文保存是被明确允许的兜底路径。
  红线：token 永不写入任何 git 仓库/日志/remote URL 配置；输出一律打码。

退出码：0 成功；1 失败；2 用法错误。全程依赖 Python 3.8+ 标准库 + git。
"""

from __future__ import annotations

import argparse
import getpass
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

SERVICE = "github-publish"  # keyring 服务名
API = "https://api.github.com"
TIMEOUT = 30


def die(msg: str, code: int = 1) -> None:
    print(f"[publish] 错误：{msg}", file=sys.stderr)
    sys.exit(code)


def log(msg: str) -> None:
    print(f"[publish] {msg}")


# ---------------------------------------------------------------- token 存取

def _config_dir() -> Path:
    if os.name == "nt" and os.environ.get("APPDATA"):
        return Path(os.environ["APPDATA"]) / SERVICE
    return Path.home() / ".config" / SERVICE


def token_file() -> Path:
    return _config_dir() / "token"


def token_load() -> str:
    tok = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if tok:
        return tok.strip()
    try:  # keyring 为可选依赖：装了就用加密存储
        import keyring  # type: ignore
        tok = keyring.get_password(SERVICE, "token")
        if tok:
            return tok
    except ImportError:
        pass
    f = token_file()
    if f.is_file():
        return f.read_text(encoding="utf-8").strip()
    die("没有可用 token：先 `github_publish.py token set`，或设 GITHUB_TOKEN 环境变量")


def token_save(tok: str, prefer_plain: bool = False) -> str:
    """保存 token，返回实际使用的存储方式描述。"""
    if not prefer_plain:
        try:
            import keyring  # type: ignore
            keyring.set_password(SERVICE, "token", tok)
            return "keyring 加密存储（系统凭据库）"
        except ImportError:
            pass
    f = token_file()
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text(tok + "\n", encoding="utf-8")
    try:  # POSIX 收紧权限；Windows 靠用户 Profile 的 ACL 保护
        f.chmod(0o600)
    except OSError:
        pass
    return f"明文文件（{f}，已设 0600；Windows 依赖用户目录 ACL）"


# ---------------------------------------------------------------- GitHub API

def api(method: str, path: str, tok: str, body: dict | None = None) -> tuple[int, dict]:
    req = urllib.request.Request(
        API + path, method=method,
        data=json.dumps(body).encode() if body is not None else None,
        headers={
            "Authorization": f"Bearer {tok}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "github-publish-script",
        })
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            data = r.read()
            return r.status, json.loads(data) if data else {}
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:300]
        return e.code, {"message": detail}


def api_or_die(method: str, path: str, tok: str, body: dict | None = None) -> dict:
    code, data = api(method, path, tok, body)
    if code >= 300:
        die(f"GitHub API {method} {path} → HTTP {code}：{data.get('message', '')}")
    return data


def redact(text: str, tok: str) -> str:
    return text.replace(tok, "***") if tok else text


# ---------------------------------------------------------------- git 辅助

def git(*args: str, cwd: Path | None = None) -> str:
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        die(f"git {' '.join(args[:3])} 失败：{(r.stderr or r.stdout).strip()[:500]}")
    return (r.stdout or "").strip()


def require_repo() -> Path:
    root = Path(git("rev-parse", "--show-toplevel"))
    if not root.is_dir():
        die("当前目录不在 git 仓库内")
    return root


def dirty_check(root: Path, allow: bool) -> None:
    if git("status", "--porcelain", cwd=root) and not allow:
        die("工作区有未提交改动 —— 先提交再推送（规则：先提交本地库，再推送 GitHub）。\n"
            "  确认要带脏区推送用 --allow-dirty（不推荐）。", )


def remote_url(owner: str, repo: str, tok: str) -> str:
    # 一次性 URL：token 只进本条命令的参数，绝不写进 .git/config
    return f"https://x:{tok}@github.com/{owner}/{repo}.git"


def repo_info_load() -> dict | None:
    f = _config_dir() / "repo.json"
    if f.is_file():
        try:
            return json.loads(f.read_text(encoding="utf-8"))
        except ValueError:
            return None
    return None


def repo_info_save(owner: str, name: str, url: str) -> None:
    f = _config_dir() / "repo.json"
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text(json.dumps({"owner": owner, "name": name, "url": url}, indent=2),
                 encoding="utf-8")


def ensure_repo_info(tok: str, owner: str | None, name: str | None) -> tuple[str, str]:
    if not (owner and name):
        # 优先从 origin remote 推导（无凭据 URL）——repo.json 是用户级全局记录，
        # 多项目共用时会串仓号，故只作兜底
        r = subprocess.run(["git", "remote", "get-url", "origin"],
                           capture_output=True, text=True, encoding="utf-8")
        if r.returncode == 0:
            m = re.search(r"github\.com[:/](.+?)/(.+?)(?:\.git)?/?$", r.stdout.strip())
            if m:
                owner, name = owner or m.group(1), name or m.group(2)
    if not (owner and name):
        info = repo_info_load()
        o = (info or {}).get("owner")
        n = (info or {}).get("name")
        owner, name = owner or o, name or n
    if not (owner and name):
        die("不知道目标仓库：用 `repo create NAME` 建仓，或 `--repo OWNER/NAME` 指定")
    repo_info_save(owner, name, f"https://github.com/{owner}/{name}")
    return owner, name


# ---------------------------------------------------------------- 子命令实现

def cmd_token(args) -> None:
    tok = sys.stdin.read().strip() if args.stdin else getpass.getpass("粘贴 GitHub token: ").strip()
    if not tok:
        die("token 为空")
    if not re.fullmatch(r"(?:ghp|gho|github_pat)_[A-Za-z0-9_]+", tok):
        die("token 格式不像 GitHub PAT（ghp_/gho_/github_pat_ 开头），拒绝保存")
    code, data = api("GET", "/user", tok)  # 先校验后落盘：坏 token 不许污染存储
    if code != 200:
        die(f"token 校验失败 HTTP {code}：{data.get('message', '')}（未保存任何内容）")
    how = token_save(tok, prefer_plain=args.plain)
    log(f"token 已保存并校验通过（{how}）：账号 {data.get('login')}")


def cmd_repo_create(args) -> None:
    tok = token_load()
    login = api_or_die("GET", "/user", tok).get("login") or die("拿不到账号名")
    body = {"name": args.name, "private": args.private,
            "description": args.desc or "", "auto_init": False}
    code, data = api("POST", "/user/repos", tok, body)
    if code == 422:  # 已存在 → 直接采用
        code, data = api("GET", f"/repos/{login}/{args.name}", tok)
        log(f"仓库已存在，直接采用：{data.get('full_name')}")
    elif code >= 300:
        die(f"建仓失败 HTTP {code}：{data.get('message', '')}")
    else:
        log(f"仓库已创建：{data.get('full_name')}（{'私有' if args.private else '公开'}）")
    repo_info_save(login, args.name, data.get("html_url") or f"https://github.com/{login}/{args.name}")
    log(f"已记录目标仓库：{login}/{args.name}")


def cmd_push(args) -> None:
    tok = token_load()
    root = require_repo()
    dirty_check(root, args.allow_dirty)
    # 开发侧文件约定：.gitignore、AGENTS.md、ledger/ 默认不上公开仓。被跟踪则提示。
    for dev in (".gitignore", "AGENTS.md"):
        if (root / dev).is_file() and git("ls-files", "--", dev, cwd=root):
            log(f"提示：{dev} 已被跟踪——公开仓惯例是移出（git rm --cached {dev} 后提交）")
    if (root / "ledger").is_dir() and git("ls-files", "--", "ledger", cwd=root):
        log("提示：ledger/ 已被跟踪——台账属开发过程文档，公开仓惯例是移出（git rm -r --cached ledger 后提交）")
    owner, name = ensure_repo_info(tok, *([None, None] if not args.repo else args.repo.split("/", 1)))
    branch = args.branch or git("rev-parse", "--abbrev-ref", "HEAD", cwd=root)
    if branch == "HEAD":
        die("当前处于 detached HEAD，用 --branch 指定目标分支")
    url = remote_url(owner, name, tok)
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
    # tag 策略：--tags 只推版本 tag（v*）；rb/* 等回退标记属开发过程产物，永不推送
    new_vtags: list[str] = []
    if args.tags:
        all_tags = sorted(git("tag", "-l", cwd=root).split())
        vtags = [t for t in all_tags if t.startswith("v")]
        others = [t for t in all_tags if t not in vtags]
        if others:
            log("提示：本地非版本 tag 不推送（回退标记等属开发过程产物）："
                + ", ".join(others[:8]) + ("…" if len(others) > 8 else ""))
        r0 = subprocess.run(["git", "ls-remote", "--tags", url], cwd=root,
                            capture_output=True, text=True, encoding="utf-8",
                            errors="replace", env=env)
        remote_tags = {ln.split()[-1][len("refs/tags/"):] for ln in (r0.stdout or "").splitlines()
                       if ln.split() and ln.split()[-1].startswith("refs/tags/")}
        new_vtags = [t for t in vtags if t not in remote_tags]
    log(f"推送 {branch} → {owner}/{name}"
        + (f"（版本 tag 新增：{', '.join(new_vtags) or '无'}；rb/* 等回退标记不推）" if args.tags else ""))
    cmd = ["git", "push", url, f"{branch}:{branch}"]
    if args.tags:
        cmd.append("refs/tags/v*:refs/tags/v*")
    r = subprocess.run(cmd, cwd=root, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", env=env)
    out = redact((r.stderr or "") + (r.stdout or ""), tok)
    if r.returncode != 0:
        die(f"推送失败：\n{out.strip()[:800]}")
    print(out.strip()[-400:])
    # remote 只存无凭据 URL，方便日常 fetch/pull
    remotes = git("remote", cwd=root)
    if "origin" not in remotes.split():
        git("remote", "add", "origin", f"https://github.com/{owner}/{name}.git", cwd=root)
        log("已设置 origin（无凭据 URL，日常 fetch/pull 走它）")
    log(f"推送完成：https://github.com/{owner}/{name}/tree/{branch}")
    # tag 事件核验：tag 与分支捆绑推送时 GitHub 偶发不触发 CI，逐个确认
    if args.tags and new_vtags:
        wf_dir = root / ".github" / "workflows"
        has_wf = wf_dir.is_dir() and any(wf_dir.glob("*.y*ml"))
        if has_wf:
            log(f"核验 tag 触发的 CI 运行（{', '.join(new_vtags)}）…")
            deadline = time.time() + 60
            pending = list(new_vtags)
            while pending and time.time() < deadline:
                runs = api_or_die("GET", f"/repos/{owner}/{name}/actions/runs?event=push&per_page=30",
                                  tok).get("workflow_runs", [])
                seen = {r.get("head_branch") for r in runs}
                pending = [t for t in pending if t not in seen]
                if pending:
                    time.sleep(10)
            if pending:
                die("tag 事件未触发 CI（与分支捆绑推送偶发）。单独重推：\n"
                    + "\n".join(f"  git push origin :refs/tags/{t} && git push origin {t}"
                                for t in pending))
            log("tag CI 触发核验通过")


def _workflow_name(root: Path, args) -> str:
    if args.workflow:
        return args.workflow
    wf_dir = root / ".github" / "workflows"
    ymls = sorted(p.name for p in wf_dir.glob("*.y*ml")) if wf_dir.is_dir() else []
    if len(ymls) == 1:
        return ymls[0]
    die("本地有多个（或没有）workflow 文件，用 --workflow 指定；本地发现：" + ", ".join(ymls or ["（无）"]))


def cmd_ci_trigger(args) -> None:
    tok = token_load()
    root = require_repo()
    owner, name = ensure_repo_info(tok, *([None, None] if not args.repo else args.repo.split("/", 1)))
    ref = args.ref or git("rev-parse", "--abbrev-ref", "HEAD", cwd=root)
    wf = _workflow_name(root, args)
    code, data = api("POST", f"/repos/{owner}/{name}/actions/workflows/{wf}/dispatches",
                     tok, {"ref": ref})
    if code == 204:
        log(f"已触发 {wf} @ {ref}：https://github.com/{owner}/{name}/actions")
    else:
        die(f"触发失败 HTTP {code}：{data.get('message', '')}"
            f"\n  常见原因：workflow 没写 on: workflow_dispatch；或该文件不在默认分支。"
            f"\n  若 CI 由 tag push 触发，改用 `push --tags` 即可。")


def cmd_ci_status(args) -> None:
    tok = token_load()
    owner, name = ensure_repo_info(tok, *([None, None] if not args.repo else args.repo.split("/", 1)))
    q = f"/repos/{owner}/{name}/actions/runs?per_page=1"
    if args.workflow:
        q += f"&event=workflow_dispatch"
    deadline = time.time() + args.timeout * 60
    while True:
        runs = api_or_die("GET", q, tok).get("workflow_runs", [])
        if not runs:
            die("查不到任何 CI 运行记录（是否还没触发？）")
        run = runs[0]
        log(f"最近运行：{run['name']} #{run['run_number']} [{run['status']}"
            + (f"/{run['conclusion']}" if run['conclusion'] else "") + f"] {run['html_url']}")
        if run["status"] == "completed":
            ok = run["conclusion"] == "success"
            log("结论：" + ("成功 ✔" if ok else f"失败（{run['conclusion']}）"))
            sys.exit(0 if ok else 1)
        if not args.watch:
            log("仍在运行（--watch 可等待结论）")
            return
        if time.time() > deadline:
            die(f"等待超时（{args.timeout} 分钟），CI 还没跑完，稍后用 ci status 再查")
        time.sleep(15)


def cmd_publish(args) -> None:
    """全流程编排：缺仓才建 → push → 有 dispatch workflow 就触发并等待。"""
    tok = token_load()
    root = require_repo()
    dirty_check(root, args.allow_dirty)
    owner, name = ensure_repo_info(tok, *([None, None] if not args.repo else args.repo.split("/", 1)))
    code, _ = api("GET", f"/repos/{owner}/{name}", tok)
    if code == 404:
        log(f"远端无 {owner}/{name}，自动建仓…")
        cmd_repo_create(argparse.Namespace(name=name, private=args.private, desc=None))
    cmd_push(argparse.Namespace(repo=f"{owner}/{name}", tags=args.tags,
                                allow_dirty=args.allow_dirty, branch=None))
    wf_dir = root / ".github" / "workflows"
    ymls = sorted(p.name for p in wf_dir.glob("*.y*ml")) if wf_dir.is_dir() else []
    if len(ymls) == 1:
        cmd_ci_trigger(argparse.Namespace(repo=f"{owner}/{name}", workflow=ymls[0], ref=None))
        cmd_ci_status(argparse.Namespace(repo=f"{owner}/{name}", watch=args.watch,
                                         workflow=ymls[0], timeout=args.timeout))
    else:
        log(f"workflow 文件 {len(ymls)} 个，跳过自动触发；tag 触发型 CI 用 `push --tags`")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.split("子命令：")[0], allow_abbrev=False)
    sub = p.add_subparsers(dest="cmd", required=True)

    t = sub.add_parser("token", help="token 管理")
    ts = t.add_subparsers(dest="tcmd", required=True)
    ts_set = ts.add_parser("set"); ts_set.add_argument("--stdin", action="store_true")
    ts_set.add_argument("--plain", action="store_true", help="跳过 keyring 直接明文保存")
    ts_set.set_defaults(func=cmd_token)
    ts_chk = ts.add_parser("check"); ts_chk.set_defaults(func=lambda a: log(
        "token 有效：" + api_or_die("GET", "/user", token_load()).get("login", "?")))

    rc = sub.add_parser("repo", help="仓库管理")
    rs = rc.add_subparsers(dest="rcmd", required=True)
    rc_c = rs.add_parser("create"); rc_c.add_argument("name")
    rc_c.add_argument("--private", action="store_true"); rc_c.add_argument("--desc")
    rc_c.set_defaults(func=cmd_repo_create)

    ps = sub.add_parser("push", help="推送当前分支")
    ps.add_argument("--tags", action="store_true"); ps.add_argument("--branch")
    ps.add_argument("--allow-dirty", action="store_true")
    ps.add_argument("--repo", help="OWNER/NAME（缺省用已记录的）")
    ps.set_defaults(func=cmd_push)

    ct = sub.add_parser("ci", help="CI 触发与状态")
    cs = ct.add_subparsers(dest="ccmd", required=True)
    ct_t = cs.add_parser("trigger"); ct_t.add_argument("--workflow"); ct_t.add_argument("--ref")
    ct_t.add_argument("--repo"); ct_t.set_defaults(func=cmd_ci_trigger)
    ct_s = cs.add_parser("status"); ct_s.add_argument("--workflow"); ct_s.add_argument("--repo")
    ct_s.add_argument("--watch", action="store_true"); ct_s.add_argument("--timeout", type=int, default=30)
    ct_s.set_defaults(func=cmd_ci_status)

    pb = sub.add_parser("publish", help="全流程：建仓(缺才建)→push→CI")
    pb.add_argument("--private", action="store_true"); pb.add_argument("--tags", action="store_true")
    pb.add_argument("--watch", action="store_true"); pb.add_argument("--allow-dirty", action="store_true")
    pb.add_argument("--repo"); pb.add_argument("--timeout", type=int, default=30)
    pb.set_defaults(func=cmd_publish)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
