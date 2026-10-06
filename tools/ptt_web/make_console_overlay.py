#!/usr/bin/env python3
#
# Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
# SPDX-License-Identifier: Apache-2.0
#
"""从 humanoid_common 仓库生成 ptt_server --console-overlay 用的目录。

    make_console_overlay.py <humanoid_common 仓库> <输出目录> [--patched REV] [--base REV]

取 --patched（默认 HEAD，即带「控制 | 语音」的分支）里 clients/web 下和 --base（默认它和 origin/main 的分叉点，
即机器人上部署的版本）不同的文件，写到输出目录；base.sha256 记下这些文件在 --base 里的 sha256。
ptt_server 只在机器人上的原文件和 base.sha256 一致时才用替换文件，所以基线选错了只会没有语音入口，不会弄坏控制台。
"""

import argparse
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("repo")
    parser.add_argument("out")
    parser.add_argument("--patched", default="HEAD")
    parser.add_argument("--base", default=None)
    args = parser.parse_args()
    base = args.base or git(args.repo, "merge-base", args.patched, "origin/main").decode().strip()
    status = [line.split("\t") for line in
              git(args.repo, "diff", "--name-status", base, args.patched, "--", "clients/web").decode().splitlines()]
    if not status:
        sys.exit(f"{args.patched} 和 {base} 的 clients/web 没有差别")
    if any(kind != "M" for kind, *_ in status):  # 控制台只认固定的几个文件名，新增 / 删除的文件替换不了
        sys.exit("只支持修改已有文件：" + ", ".join("\t".join(item) for item in status if item[0] != "M"))
    changed = [path for _, path in status]
    out = Path(args.out)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    lines = []
    for path in changed:
        name = path[len("clients/web/"):]
        if "/" in name:
            sys.exit(f"只支持 clients/web 下一层的文件：{path}")
        original = git(args.repo, "show", f"{base}:{path}")
        (out / name).write_bytes(git(args.repo, "show", f"{args.patched}:{path}"))
        lines.append(f"{hashlib.sha256(original).hexdigest()}  {name}")
    (out / "base.sha256").write_text("\n".join(lines) + "\n")
    print(f"{out}: {', '.join(p[len('clients/web/'):] for p in changed)}（基线 {base[:12]}）")


if __name__ == "__main__":
    main()
