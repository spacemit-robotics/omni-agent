#!/usr/bin/env python3
# Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
# SPDX-License-Identifier: Apache-2.0
"""ptt_server.py 端到端测试：用假的 ptt_chat 按步骤发事件，检查页面拿到的对话状态。

覆盖：静态文件和 CSP、api/info、按住互斥、回答别人时不让按、边生成边显示、准备做动作 → 已做、
被打断时留下已显示的文字、空闲清空记忆的分隔标记、纯 HTTP 跳转带上 back 参数、退出时让 ptt_chat 退出。
"""

import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

SERVER = Path(__file__).resolve().parent.parent / "tools" / "ptt_web" / "ptt_server.py"

# 假 ptt_chat：第 1 段录音分四步回答，每步等测试放行（gate-a / b / c）；第 2 段录音说一半被打断
FAKE_CHAT = r'''
import json, os, sys, time
work = sys.argv[1]
fd = int(sys.argv[sys.argv.index("--event-fd") + 1])
out = os.fdopen(fd, "w", buffering=1)
def emit(**event):
    out.write(json.dumps(event, ensure_ascii=False) + "\n")
def gate(name):
    while not os.path.exists(os.path.join(work, name)):
        time.sleep(0.02)
def note(line):
    with open(os.path.join(work, "commands"), "a") as f:
        f.write(line + "\n")
emit(type="ready")
turn = 0
for line in sys.stdin:
    line = line.strip()
    note(line.split(" ")[0])
    if line == "quit":
        break
    if not line.startswith("wav "):
        continue
    turn += 1
    emit(type="state", id=turn, state="recognizing")
    if turn == 1:
        emit(type="asr", id=1, text="挥个手", ms=120)
        emit(type="state", id=1, state="thinking")
        emit(type="delta", id=1, text="好的，")
        gate("gate-a")
        emit(type="delta", id=1, text="我来挥手。")
        emit(type="tool", id=1, name="perform_action", args={"action": "wave"})
        gate("gate-b")
        emit(type="tool_result", id=1, name="perform_action", text="已开始做「挥手」。")
        gate("gate-c")
        emit(type="reply", id=1, text="好的，我来挥手。", tools=["已开始做「挥手」。"], interrupted=False)
    else:
        emit(type="asr", id=turn, text="讲个笑话", ms=90)
        emit(type="state", id=turn, state="thinking")
        emit(type="delta", id=turn, text="从前有")
        emit(type="reply", id=turn, text="", tools=[], interrupted=True)
    emit(type="state", id=turn, state="idle")
'''

WAV = b"RIFF" + (36 + 3200).to_bytes(4, "little") + b"WAVEfmt " + bytes(28) + b"data" + (3200).to_bytes(4, "little") \
    + bytes(3200)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def check(condition, message):
    if not condition:
        raise AssertionError(message)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def request(url, method="GET", body=None):
    req = urllib.request.Request(url, data=body, method=method)
    try:
        with urllib.request.build_opener(NoRedirect).open(req, timeout=10) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def main():
    work = Path(tempfile.mkdtemp(prefix="ptt-web-test."))
    (work / "fake_chat.py").write_text(FAKE_CHAT)
    (work / "token").write_text("testtoken1234567\n")
    port, redirect_port = free_port(), free_port()
    base = f"http://127.0.0.1:{port}/testtoken1234567/"
    server = subprocess.Popen(
        [sys.executable, str(SERVER), "--listen", f"127.0.0.1:{port}", "--token-file", str(work / "token"),
         "--upload-dir", str(work / "up"), "--idle-reset", "2", "--robot-name", "灵龙",
         "--actions", "wave=挥手,heart=比心",
         "--redirect-listen", f"127.0.0.1:{redirect_port}", "--redirect-to", base,
         "--", sys.executable, str(work / "fake_chat.py"), str(work)],
        stdout=open(work / "server.log", "w"), stderr=subprocess.STDOUT)

    def api(path, client, method="GET", body=None, since=-1):
        extra = f"&since={since}" if path == "status" else ""
        code, _, data = request(f"{base}api/{path}?client={client}{extra}", method, body)
        check(code == 200, f"api/{path} -> {code}")
        return json.loads(data)

    def wait(client, predicate, what, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                status = api("status", client)
                if predicate(status):
                    return status
            except (OSError, AssertionError):
                pass
            time.sleep(0.05)
        raise AssertionError(f"等不到: {what}")

    a, b = "clientaaaa01", "clientbbbb02"
    try:
        wait(a, lambda s: s["ready"], "ptt_chat 就绪")

        # 页面文件、安全头
        for path, kind in (("", "text/html"), ("app.js", "javascript"), ("style.css", "text/css"),
                           ("logo.svg", "image/svg+xml")):
            code, headers, _ = request(base + path)
            check(code == 200 and kind in headers["Content-Type"], f"{path or 'index'}: {code} {headers}")
            check("default-src 'self'" in headers["Content-Security-Policy"], f"{path}: 没有 CSP")
        check(request(f"http://127.0.0.1:{port}/wrongtoken12345/")[0] == 404, "错误 token 应 404")

        info = api("info", a)
        check(info == {"robot": "灵龙", "actions": ["挥手", "比心"]}, f"api/info: {info}")

        # 按住互斥
        check(api("press", a, "POST")["ok"], "A 按下")
        r = api("press", b, "POST")
        check(not r["ok"] and "别人正在说话" in r["error"], f"B 应被拒: {r}")
        check(api("status", b)["holder"] == "other", "B 看到别人按着")
        r = api("audio", a, "POST", WAV)
        check(r["ok"] and r["id"] == 1, f"A 上传: {r}")

        # 边生成边显示；别人看到灰色的「其他人」、不能按
        s = wait(a, lambda s: s["history"] and s["history"][-1]["live"] == "好的，", "第一段文字")
        turn = s["history"][-1]
        check(s["state"] == "thinking" and s["live_id"] == 1 and turn["mine"], f"A 视角: {s}")
        check(turn["heard"] == "挥个手" and turn["reply"] is None and not s["answering_other"], f"A 视角: {turn}")
        sb = api("status", b)
        check(not sb["history"][-1]["mine"] and sb["answering_other"], f"B 视角: {sb}")
        r = api("press", b, "POST")
        check(not r["ok"] and "回答别人" in r["error"], f"回答 A 时 B 应被拒: {r}")

        # 模型决定做动作：先显示「准备做」，做了之后变成结果
        (work / "gate-a").touch()
        s = wait(a, lambda s: s["history"][-1]["pending"], "准备做动作")
        turn = s["history"][-1]
        check(turn["pending"] == ["准备做「挥手」"] and turn["live"] == "好的，我来挥手。", f"pending: {turn}")
        (work / "gate-b").touch()
        s = wait(a, lambda s: s["history"][-1]["tools"], "动作结果")
        turn = s["history"][-1]
        check(turn["tools"] == ["已开始做「挥手」。"] and not turn["pending"] and turn["reply"] is None, f"动作结果: {turn}")
        (work / "gate-c").touch()
        s = wait(a, lambda s: s["state"] == "idle", "第一轮结束")
        turn = s["history"][-1]
        check(turn["reply"] == "好的，我来挥手。" and turn["tools"] == ["已开始做「挥手」。"] and not turn["pending"],
              f"第一轮结果: {turn}")
        check(not s["answering_other"] and not api("status", b)["answering_other"], "回答完 B 可以按")

        # B 说话被打断：回答为空，但已经显示的文字留着
        check(api("press", b, "POST")["ok"], "B 按下")
        check(api("audio", b, "POST", WAV)["id"] == 2, "B 上传")
        s = wait(a, lambda s: len(s["history"]) == 2 and s["state"] == "idle", "第二轮结束")
        turn = s["history"][-1]
        check(turn["reply"] == "" and turn["live"] == "从前有" and turn["interrupted"] and not turn["mine"],
              f"第二轮: {turn}")
        check(api("status", b)["history"][-1]["mine"], "B 视角第二轮是自己的")

        # 长轮询：since 等于当前 seq 时挂住，直到超时或有变化
        seq = api("status", a)["seq"]
        started = time.monotonic()
        later = api("status", a, since=seq)  # 空闲清空记忆会在 2~4 s 内触发一次变化
        check(time.monotonic() - started > 0.5 and later["seq"] > seq, "长轮询应挂到有变化")

        # 空闲清空记忆：分隔线标在最后一轮后面，ptt_chat 收到 reset
        s = wait(a, lambda s: s["resets"], "空闲清空记忆", timeout=10)
        check(s["resets"] == [2], f"resets: {s['resets']}")
        check("reset" in (work / "commands").read_text().split(), "ptt_chat 应收到 reset")

        # 纯 HTTP 入口跳到页面，带上 back
        query = "back=http%3A%2F%2F192.168.1.247%3A8765%2F"
        code, headers, _ = request(f"http://127.0.0.1:{redirect_port}/?{query}")
        check(code == 302 and headers["Location"] == f"{base}?{query}", f"跳转: {code} {headers.get('Location')}")
        code, headers, _ = request(f"http://127.0.0.1:{redirect_port}/")
        check(code == 302 and headers["Location"] == base, f"不带参数的跳转: {headers.get('Location')}")
    except AssertionError as e:
        print(f"FAIL: {e}\n--- {work}/server.log ---\n{(work / 'server.log').read_text()}", file=sys.stderr)
        server.kill()
        sys.exit(1)

    server.send_signal(signal.SIGTERM)
    code = server.wait(timeout=10)
    commands = (work / "commands").read_text().split()
    check(code == 0 and commands[-1] == "quit", f"退出: code {code}, 命令 {commands}")
    shutil.rmtree(work)
    print("ptt_web: all checks passed")


if __name__ == "__main__":
    main()
