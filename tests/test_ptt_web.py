#!/usr/bin/env python3
# Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
# SPDX-License-Identifier: Apache-2.0
"""ptt_server.py 端到端测试：假的机器人控制台 + 假的 ptt_chat，检查转发和语音面板拿到的对话状态。

转发：控制台页面原样转发（Host 不改）、替换网页文件只在原文件和基线一致时生效（他们升级后原样转发）、
只给本机的配对入口不转发（伪造 Host 也不行）、WebSocket 双向转发（明文和 TLS 各一遍，TLS 下用跨多个记录的大消息）、
控制台挂了回 502。
语音：面板文件、接口要带配对令牌（重新配对后旧令牌失效）、按住互斥、回答别人时不让按、边生成边显示、
准备做动作 → 已做、被打断时留下已显示的文字、空闲清空记忆的分隔标记、纯 HTTP 跳转带上查询参数、退出时让 ptt_chat 退出。
ptt_chat 起不来或中途退出（比如没网）：控制台照常转发，语音显示不可用，隔一会儿重启；重启后对话编号接着排。
"""

import hashlib
import json
import os
import shutil
import signal
import socket
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

SERVER = Path(__file__).resolve().parent.parent / "tools" / "ptt_web" / "ptt_server.py"

FAKE_CHAT = r'''
import json, os, sys, time
work = sys.argv[1]
fd = int(sys.argv[sys.argv.index("--event-fd") + 1])
out = os.fdopen(fd, "w", buffering=1)
def emit(**event):
    out.write(json.dumps(event, ensure_ascii=False) + "\n")
def gate(name):
    deadline = time.monotonic() + 30  # 测试失败时不放行：到点自己退出，不留孤儿进程
    while not os.path.exists(os.path.join(work, name)):
        if time.monotonic() > deadline:
            sys.exit(1)
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
        emit(type="tool", id=1, name="get_weather", args={"city": "深圳"})
        gate("gate-w")
        emit(type="tool_result", id=1, name="get_weather", text="深圳现在晴，气温 30 度。")
        emit(type="tool", id=1, name="perform_action", args={"action": "wave"})
        gate("gate-b")
        emit(type="tool_result", id=1, name="perform_action", text="已开始做「挥手」。")
        gate("gate-c")
        emit(type="reply", id=1, text="好的，我来挥手。", tools=["深圳现在晴，气温 30 度。", "已开始做「挥手」。"],
             interrupted=False)
    else:
        emit(type="asr", id=turn, text="讲个笑话", ms=90)
        emit(type="state", id=turn, state="thinking")
        emit(type="delta", id=turn, text="从前有")
        emit(type="reply", id=turn, text="", tools=[], interrupted=True)
    emit(type="state", id=turn, state="idle")
'''

# 起不来、做完一轮就退出的 ptt_chat：work/fail 存在时直接失败（像开机没网时连不上大模型）；回答里带上总共第几轮
CRASHY_CHAT = r'''
import json, os, sys
work = sys.argv[1]
out = os.fdopen(int(sys.argv[sys.argv.index("--event-fd") + 1]), "w", buffering=1)
def emit(**event):
    out.write(json.dumps(event, ensure_ascii=False) + "\n")
with open(os.path.join(work, "starts"), "a") as f:
    f.write("x")
if os.path.exists(os.path.join(work, "fail")):
    sys.exit(1)
emit(type="ready")
for line in sys.stdin:
    if not line.startswith("wav "):
        continue
    count = os.path.join(work, "count")
    n = int(open(count).read()) + 1 if os.path.exists(count) else 1
    open(count, "w").write(str(n))
    emit(type="state", id=1, state="recognizing")
    emit(type="asr", id=1, text=f"第{n}句", ms=10)
    emit(type="reply", id=1, text=f"第{n}次回答", tools=[], interrupted=False)
    emit(type="state", id=1, state="idle")
    sys.exit(3)
'''


WAV = b"RIFF" + (36 + 3200).to_bytes(4, "little") + b"WAVEfmt " + bytes(28) + b"data" + (3200).to_bytes(4, "little") \
    + bytes(3200)


class FakeConsole(socketserver.ThreadingTCPServer):
    """假的操作服务网页：每个连接一个请求后关闭（和真的一样）；配对入口照真的逻辑对本机发令牌；WebSocket 回显。"""

    daemon_threads = True
    allow_reuse_address = True

    def __init__(self):
        self.seen = []  # (目标, Host)
        self.index = b"<!doctype html><title>console</title>"
        super().__init__(("127.0.0.1", 0), FakeConsoleHandler)


class FakeConsoleHandler(socketserver.StreamRequestHandler):
    def handle(self):
        lines = []
        while True:
            line = self.rfile.readline()
            if line in (b"\r\n", b"\n", b""):
                break
            lines.append(line.decode("latin-1").rstrip("\r\n"))
        if not lines:
            return
        method, target, _ = lines[0].split(" ", 2)
        headers = {k.lower(): v.strip() for k, v in (h.split(":", 1) for h in lines[1:])}
        self.server.seen.append((target, headers.get("host", "")))
        if target == "/api/v1/ws" and headers.get("upgrade", "").lower() == "websocket":
            self.wfile.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n")
            while True:
                data = self.connection.recv(4096)
                if not data:
                    return
                self.connection.sendall(data[::-1])  # 每块倒序回显：收到的一定是控制台处理过的，不是原样折回
        if target == "/api/v1/local-session":
            body = b'{"token":"console-secret"}'  # 真的服务对本机对端 + localhost Host 才发；这里一律发，看代理拦不拦
        elif target == "/" and method == "GET":
            body = self.server.index
        else:
            self.wfile.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            return
        self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s" % (len(body), body))


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


def request(url, method="GET", body=None, headers=None):
    req = urllib.request.Request(url, data=body, method=method, headers=headers or {})
    try:
        with urllib.request.build_opener(NoRedirect).open(req, timeout=10) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def main():
    work = Path(tempfile.mkdtemp(prefix="ptt-web-test."))
    (work / "fake_chat.py").write_text(FAKE_CHAT)
    console = FakeConsole()
    threading.Thread(target=console.serve_forever, daemon=True).start()
    console_port = console.server_address[1]
    # 替换网页：首页的基线是假控制台现在的首页；app.js 的基线随便写（假控制台对它回 404，应原样转发）
    overlay = work / "overlay"
    overlay.mkdir()
    (overlay / "index.html").write_bytes(b"<!doctype html><title>voice console</title>")
    (overlay / "app.js").write_bytes(b"// voice")
    (overlay / "base.sha256").write_text(
        f"{hashlib.sha256(console.index).hexdigest()}  index.html\n{'0' * 64}  app.js\n")
    connection = work / "connection.json"
    connection.write_text(json.dumps({"v": 1, "endpoint": f"ws://127.0.0.1:{console_port}", "token": "optoken-one"}))
    port, redirect_port = free_port(), free_port()
    base = f"http://127.0.0.1:{port}"
    server = subprocess.Popen(
        [sys.executable, str(SERVER), "--listen", f"127.0.0.1:{port}", "--operator-connection", str(connection),
         "--console", f"127.0.0.1:{console_port}", "--console-overlay", str(overlay),
         "--upload-dir", str(work / "up"), "--idle-reset", "2",
         "--actions", "wave=挥手,heart=比心",
         "--redirect-listen", f"127.0.0.1:{redirect_port}", "--redirect-to", "https://robot.example:8443/",
         "--", sys.executable, str(work / "fake_chat.py"), str(work)],
        stdout=open(work / "server.log", "w"), stderr=subprocess.STDOUT, start_new_session=True)
    token = {"X-Operator-Token": "optoken-one"}
    # 同一个假控制台再起一个 TLS 实例（机器人上就是这样跑的），只测转发
    tls_port = free_port()
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=localhost",
                    "-keyout", str(work / "key.pem"), "-out", str(work / "cert.pem")], check=True, capture_output=True)
    (work / "tls").mkdir()
    tls_server = subprocess.Popen(
        [sys.executable, str(SERVER), "--listen", f"127.0.0.1:{tls_port}", "--operator-connection", str(connection),
         "--console", f"127.0.0.1:{console_port}", "--upload-dir", str(work / "tls" / "up"),
         "--tls-cert", str(work / "cert.pem"), "--tls-key", str(work / "key.pem"),
         "--", sys.executable, str(work / "fake_chat.py"), str(work / "tls")],
        stdout=open(work / "tls_server.log", "w"), stderr=subprocess.STDOUT, start_new_session=True)
    insecure = ssl.create_default_context()
    insecure.check_hostname = False
    insecure.verify_mode = ssl.CERT_NONE

    def api(path, client, method="GET", body=None, since=-1, headers=token):
        extra = f"&since={since}" if path == "status" else ""
        code, _, data = request(f"{base}/voice/api/{path}?client={client}{extra}", method, body, headers)
        check(code == 200, f"voice/api/{path} -> {code}")
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

        # 控制台页面原样转发，Host 是浏览器看到的（控制台据此做跨站检查）；首页和基线一致，换成语音版
        code, headers, body = request(base + "/")
        check(code == 200 and body == b"<!doctype html><title>voice console</title>", f"替换首页: {code} {body[:60]}")
        check(headers["Content-Length"] == str(len(body)), f"替换后长度: {headers}")
        check(console.seen[-1] == ("/", f"127.0.0.1:{port}"), f"控制台收到的 Host: {console.seen[-1]}")
        check(request(base + "/nope")[0] == 404, "控制台的 404 原样回来")
        check(request(base + "/app.js")[0] == 404, "要替换的文件控制台回 404 时原样回来")
        # 他们升级了首页：和基线对不上，原样转发（没有语音，但控制台能用），日志里提醒
        console.index = b"<!doctype html><title>console v2</title>"
        code, _, body = request(base + "/")
        check(code == 200 and body == console.index, f"基线对不上应原样转发: {body[:60]}")
        check("不一致" in (work / "server.log").read_text(), "基线对不上要记日志")
        console.index = b"<!doctype html><title>console</title>"

        # 只给本机的配对入口一律不转发，伪造成 localhost 的 Host 也不行
        before = len(console.seen)
        for target, host in (("/api/v1/local-session", None), ("/api/v1/local-session", f"127.0.0.1:{console_port}"),
                             ("/api/v1/local-session?x=1", "localhost"), ("/pairing.svg", f"127.0.0.1:{console_port}")):
            code, _, body = request(base + target, headers={"Host": host} if host else None)
            check(code == 404 and b"console-secret" not in body, f"{target} Host={host} 应被拦: {code} {body}")
        check(len(console.seen) == before, f"被拦的请求不该到控制台: {console.seen[before:]}")

        # WebSocket：升级后两头直接对接，来回多次
        with socket.create_connection(("127.0.0.1", port), timeout=5) as ws:
            ws.sendall(f"GET /api/v1/ws HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n"
                       f"Connection: Upgrade\r\nSec-WebSocket-Key: x\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
            head = ws.recv(4096)
            check(head.startswith(b"HTTP/1.1 101"), f"WebSocket 升级: {head[:40]}")
            for message in (b"hello", b"renew" * 100):
                ws.sendall(message)
                got = b""
                while len(got) < len(message):
                    got += ws.recv(65536)
                check(got == message[::-1], f"WebSocket 回显: {got[:40]}")

        # TLS：首页和大消息 WebSocket（64 KB 跨多个 TLS 记录，解密后留在缓冲里的部分 select 看不到）
        for _ in range(50):
            try:
                with socket.create_connection(("127.0.0.1", tls_port), timeout=1):
                    break
            except OSError:
                time.sleep(0.1)
        with urllib.request.urlopen(f"https://127.0.0.1:{tls_port}/", context=insecure, timeout=10) as r:
            check(r.status == 200 and b"<title>console</title>" in r.read(), "TLS 转发首页")
        raw = socket.create_connection(("127.0.0.1", tls_port), timeout=5)
        with insecure.wrap_socket(raw, server_hostname="localhost") as ws:
            ws.sendall(f"GET /api/v1/ws HTTP/1.1\r\nHost: 127.0.0.1:{tls_port}\r\nUpgrade: websocket\r\n"
                       f"Connection: Upgrade\r\n\r\n".encode())
            head = b""
            while b"\r\n\r\n" not in head:
                head += ws.recv(4096)
            check(head.startswith(b"HTTP/1.1 101"), f"TLS WebSocket 升级: {head[:40]}")
            for message in (bytes(range(256)) * 256, b"small"):  # 64 KB，倒序回显后仍按块可比
                ws.sendall(message)
                got = b""
                while len(got) < len(message):
                    got += ws.recv(65536)
                check(len(got) == len(message) and sorted(got) == sorted(message),
                      f"TLS WebSocket 回显 {len(message)} 字节: 收到 {len(got)}")
        try:  # 明文连 TLS 端口：握手失败断开，服务照常
            request(f"http://127.0.0.1:{tls_port}/")
        except OSError:
            pass
        with urllib.request.urlopen(f"https://127.0.0.1:{tls_port}/", context=insecure, timeout=10) as r:
            check(r.status == 200, "明文乱连之后 TLS 照常")

        # 语音面板文件
        for path, kind in (("panel.js", "javascript"), ("panel.css", "text/css")):
            code, headers, _ = request(f"{base}/voice/{path}")
            check(code == 200 and kind in headers["Content-Type"], f"{path}: {code} {headers}")
        check(request(f"{base}/voice/nope")[0] == 404, "/voice/ 下没有的文件 404，不转发")

        # 语音接口要配对令牌
        for headers in ({}, {"X-Operator-Token": "wrong"}):
            code, _, body = request(f"{base}/voice/api/status?client={a}", headers=headers)
            check(code == 401 and "配对" in json.loads(body)["error"], f"没令牌应 401: {code} {body}")
            code, _, _ = request(f"{base}/voice/api/press?client={a}", "POST", b"", headers)
            check(code == 401, f"没令牌按下应 401: {code}")
        info = api("info", a)
        check(info == {"actions": ["挥手", "比心"]}, f"api/info: {info}")

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

        # 查天气只在查询时提示，结果不显示（机器人会说出来）
        (work / "gate-a").touch()
        s = wait(a, lambda s: s["history"][-1]["lookups"], "查天气")
        turn = s["history"][-1]
        check(turn["lookups"] == ["正在查深圳的天气"] and not turn["pending"] and not turn["tools"], f"查询中: {turn}")

        # 模型决定做动作：先显示「准备做」，做了之后变成结果
        (work / "gate-w").touch()
        s = wait(a, lambda s: s["history"][-1]["pending"], "准备做动作")
        turn = s["history"][-1]
        check(turn["pending"] == ["准备做「挥手」"] and turn["live"] == "好的，我来挥手。" and not turn["lookups"]
              and not turn["tools"], f"pending: {turn}")
        (work / "gate-b").touch()
        s = wait(a, lambda s: s["history"][-1]["tools"], "动作结果")
        turn = s["history"][-1]
        check(turn["tools"] == ["已开始做「挥手」。"] and not turn["pending"] and turn["reply"] is None,
              f"动作结果: {turn}")
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

        # 重新配对换了令牌：旧令牌失效，新令牌可用
        time.sleep(0.01)
        connection.write_text(json.dumps({
            "v": 1, "endpoint": f"ws://127.0.0.1:{console_port}", "token": "optoken-two"}))
        code, _, _ = request(f"{base}/voice/api/info?client={a}", headers=token)
        check(code == 401, f"旧令牌应失效: {code}")
        check(api("info", a, headers={"X-Operator-Token": "optoken-two"})["actions"], "新令牌可用")

        connection.write_text(json.dumps({"v": 1, "token": "private-control", "public_token": "optoken-two"}))
        check(api("info", a, headers={"X-Operator-Token": "optoken-two"})["actions"], "公开语音令牌仍可用")
        code, _, _ = request(f"{base}/voice/api/info?client={a}", headers={"X-Operator-Token": "private-control"})
        check(code == 401, "语音接口不接受本机控制令牌")

        # 纯 HTTP 入口跳到 HTTPS 页面，带上查询参数（#token= 片段由浏览器自己保留）
        code, headers, _ = request(f"http://127.0.0.1:{redirect_port}/?mode=voice")
        check(code == 302 and headers["Location"] == "https://robot.example:8443/?mode=voice",
              f"跳转: {headers.get('Location')}")

        # 控制台挂了：502，不挂住
        console.shutdown()
        console.server_close()
        code, _, _ = request(base + "/")
        check(code == 502, f"控制台挂了应 502: {code}")
    except Exception as e:  # noqa: BLE001  任何失败都要把服务和假 ptt_chat 整组杀掉
        print(f"FAIL: {type(e).__name__}: {e}\n--- {work}/server.log ---\n{(work / 'server.log').read_text()}",
              file=sys.stderr)
        os.killpg(server.pid, signal.SIGKILL)
        os.killpg(tls_server.pid, signal.SIGKILL)
        sys.exit(1)

    tls_server.send_signal(signal.SIGTERM)
    tls_server.wait(timeout=10)
    server.send_signal(signal.SIGTERM)
    code = server.wait(timeout=10)
    commands = (work / "commands").read_text().split()
    check(code == 0 and commands[-1] == "quit", f"退出: code {code}, 命令 {commands}")
    shutil.rmtree(work)
    restart_checks()
    print("ptt_web: all checks passed")


def restart_checks():
    work = Path(tempfile.mkdtemp(prefix="ptt-web-restart."))
    (work / "chat.py").write_text(CRASHY_CHAT)
    (work / "fail").touch()
    console = FakeConsole()
    threading.Thread(target=console.serve_forever, daemon=True).start()
    connection = work / "connection.json"
    connection.write_text(json.dumps({"v": 1, "endpoint": f"ws://127.0.0.1:{console.server_address[1]}", "token": "t"}))
    port = free_port()
    base = f"http://127.0.0.1:{port}"
    server = subprocess.Popen(
        [sys.executable, str(SERVER), "--listen", f"127.0.0.1:{port}", "--operator-connection", str(connection),
         "--console", f"127.0.0.1:{console.server_address[1]}", "--upload-dir", str(work / "up"),
         "--restart-delay", "0.3", "--", sys.executable, str(work / "chat.py"), str(work)],
        stdout=open(work / "server.log", "w"), stderr=subprocess.STDOUT, start_new_session=True)
    token = {"X-Operator-Token": "t"}

    def status():
        code, _, data = request(f"{base}/voice/api/status?client=restartcli01&since=-1", headers=token)
        check(code == 200, f"status -> {code}")
        return json.loads(data)

    def wait(predicate, what, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                s = status()
                if predicate(s):
                    return s
            except (OSError, AssertionError):
                pass
            time.sleep(0.05)
        raise AssertionError(f"等不到: {what}")

    def talk():
        code, _, data = request(f"{base}/voice/api/press?client=restartcli01", "POST", b"", token)
        check(code == 200 and json.loads(data)["ok"], f"按下: {data}")
        code, _, data = request(f"{base}/voice/api/audio?client=restartcli01", "POST", WAV, token)
        check(code == 200 and json.loads(data)["ok"], f"上传: {data}")
        return json.loads(data)["id"]

    try:
        # 一直起不来：一直在重启，控制台照常转发，语音显示不可用、不让按
        deadline = time.monotonic() + 8
        while len((work / "starts").read_text() if (work / "starts").exists() else "") < 3:
            check(time.monotonic() < deadline, "ptt_chat 起不来时应反复重启")
            time.sleep(0.05)
        code, _, body = request(base + "/")
        check(code == 200 and body == console.index, f"ptt_chat 起不来时控制台照常转发: {code}")
        s = status()
        check(not s["ready"], f"起不来时语音不可用: {s}")
        code, _, data = request(f"{base}/voice/api/press?client=restartcli01", "POST", b"", token)
        check(code == 200 and not json.loads(data)["ok"], f"起不来时不让按: {data}")

        # 网络好了：下一次重启就起来；一轮之后它退出、再重启，第二轮的编号接着排，不覆盖第一轮
        (work / "fail").unlink()
        wait(lambda s: s["ready"], "恢复后 ptt_chat 就绪")
        starts_before_talk = len((work / "starts").read_text())
        check(talk() == 1, "第一轮编号 1")
        s = wait(lambda s: s["history"] and s["history"][-1]["reply"], "第一轮回答")
        check([t["reply"] for t in s["history"]] == ["第1次回答"], f"第一轮: {s['history']}")
        wait(lambda s: len((work / "starts").read_text()) > starts_before_talk
             and s["ready"] and not s["holder"], "退出后重启就绪")
        check(talk() == 2, "重启后编号接着排")
        s = wait(lambda s: len(s["history"]) == 2 and s["history"][-1]["reply"], "第二轮回答")
        expected = [(1, "第1句", "第1次回答"), (2, "第2句", "第2次回答")]
        check([(t["id"], t["heard"], t["reply"]) for t in s["history"]] == expected,
              f"重启前后的对话: {s['history']}")
    except Exception as e:  # noqa: BLE001
        print(f"FAIL: {type(e).__name__}: {e}\n--- {work}/server.log ---\n{(work / 'server.log').read_text()}",
              file=sys.stderr)
        os.killpg(server.pid, signal.SIGKILL)
        sys.exit(1)
    server.send_signal(signal.SIGTERM)
    check(server.wait(timeout=10) == 0, "重启过的服务也能正常退出")
    console.shutdown()
    console.server_close()
    shutil.rmtree(work)


if __name__ == "__main__":
    main()
