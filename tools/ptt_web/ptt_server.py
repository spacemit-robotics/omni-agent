#!/usr/bin/env python3
#
# Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
# SPDX-License-Identifier: Apache-2.0
#
"""按住说话网页：手机按住按钮录音，松手上传，交给 ptt_chat 识别、回答并用本机扬声器播放。

    ptt_server.py [--listen 127.0.0.1:18080] [--token-file F] [--tls-cert C --tls-key K]
                  [--redirect-listen [::]:8442 --redirect-to URL] -- ptt_chat ...

页面和接口都在 /<token>/ 下，其他路径一律 404。一次只让一个人说；机器人回答某个人时，别人要等它说完。
手机浏览器只在 HTTPS 下允许录音：给了 --tls-cert/--tls-key 时本服务直接提供 HTTPS（例如在局域网监听），
否则只监听本机，由前面的反向代理提供 HTTPS。
"""

import argparse
import json
import os
import secrets
import signal
import socket
import ssl
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

HERE = Path(__file__).resolve().parent
STATIC = {"": ("index.html", "text/html; charset=utf-8"), "app.js": ("app.js", "text/javascript; charset=utf-8")}
MAX_UPLOAD = 2 * 1024 * 1024  # 16 kHz 单声道 16 位约 60 s
KEEP_UPLOADS = 20
CSP = ("default-src 'self'; media-src 'self' blob:; style-src 'self' 'unsafe-inline'; base-uri 'none'; "
       "form-action 'none'")


def log(message):
    print(time.strftime("%H:%M:%S") + " [ptt_web] " + message, flush=True)


class Robot:
    """ptt_chat 子进程和对话状态。所有状态由 cond 保护，seq 每次变化加一，供长轮询等待。"""

    def __init__(self, command, upload_dir, max_hold_s, idle_reset_s, chat_log):
        self.upload_dir = upload_dir
        self.max_hold_s = max_hold_s
        self.idle_reset_s = idle_reset_s
        self.cond = threading.Condition()
        self.seq = 0
        self.ready = False
        self.alive = True
        self.state = "idle"
        self.holder = None          # 正在按住说话的客户端
        self.hold_until = 0.0
        self.next_id = 0            # 和 ptt_chat 的任务编号一一对应：只有 wav 命令会占号
        self.turns = {}             # id -> {owner, heard, reply, tools, started}
        self.last_activity = time.monotonic()
        read_fd, write_fd = os.pipe()
        output = open(chat_log, "a", encoding="utf-8") if chat_log else None  # 子进程一直用到退出
        self.proc = subprocess.Popen(command + ["--event-fd", str(write_fd)], stdin=subprocess.PIPE,
                                     stdout=output, stderr=subprocess.STDOUT if output else None,
                                     pass_fds=(write_fd,), text=True, bufsize=1)
        os.close(write_fd)
        self.events = os.fdopen(read_fd, "r", encoding="utf-8", errors="replace")
        threading.Thread(target=self._read_events, daemon=True).start()
        if idle_reset_s > 0:
            threading.Thread(target=self._idle_reset, daemon=True).start()

    def _changed(self):
        self.seq += 1
        self.cond.notify_all()

    def _send(self, line):
        try:
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, ValueError):
            pass

    def _read_events(self):
        for line in self.events:
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            with self.cond:
                kind = event.get("type")
                turn = self.turns.get(event.get("id"))
                if kind == "ready":
                    self.ready = True
                    log("ptt_chat 就绪")
                elif kind == "state":
                    if event.get("id") == self.next_id:
                        self.state = event.get("state", "idle")
                elif kind == "asr" and turn is not None:
                    turn["heard"] = event.get("text", "")
                    log(f"#{event['id']} 识别 {event.get('ms')} ms: {turn['heard']!r}")
                elif kind == "reply" and turn is not None:
                    turn["reply"] = event.get("text", "")
                    turn["tools"] = event.get("tools", [])
                    turn["interrupted"] = event.get("interrupted", False)
                    log(f"#{event['id']} 回答: {turn['reply']!r} {turn['tools']}")
                elif kind == "error" and turn is not None:
                    turn["error"] = event.get("message", "")
                self.last_activity = time.monotonic()
                self._changed()
        code = self.proc.wait()
        log(f"ptt_chat 退出 (code {code})")
        with self.cond:
            self.alive = False
            self.ready = False
            self._changed()

    def _idle_reset(self):
        reset_done = True
        while self.alive:
            time.sleep(5)
            with self.cond:
                idle = self.state == "idle" and self.holder is None
                quiet = time.monotonic() - self.last_activity > self.idle_reset_s
                if not (idle and quiet):
                    reset_done = False
                elif not reset_done:
                    self._send("reset")
                    reset_done = True
                    log(f"空闲 {self.idle_reset_s} s，清空对话记忆")

    def _holder_active(self):
        if self.holder is not None and time.monotonic() > self.hold_until:
            log(f"{self.holder} 按住超时，释放")
            self.holder = None
        return self.holder

    def press(self, client):
        with self.cond:
            if not self.ready:
                return False, "机器人还没准备好，请稍等"
            holder = self._holder_active()
            if holder not in (None, client):
                return False, "别人正在说话，请稍等"
            turn = self.turns.get(self.next_id)
            answering = self.state != "idle" and turn is not None and turn["owner"] != client
            if answering and time.monotonic() - turn["started"] < 60:
                return False, "机器人正在回答别人，请稍等"
            self.holder = client
            self.hold_until = time.monotonic() + self.max_hold_s + 10
            self._send("interrupt")  # 按下就打断：停止播放、取消正在进行的回答
            self.last_activity = time.monotonic()
            self._changed()
            return True, ""

    def release(self, client):
        with self.cond:
            if self.holder == client:
                self.holder = None
                self._changed()

    def submit(self, client, wav):
        with self.cond:
            if self._holder_active() != client:
                return None, "没有按住说话，或按得太久了"
            self.next_id += 1
            turn_id = self.next_id
            path = self.upload_dir / f"{turn_id:05d}.wav"
            path.write_bytes(wav)
            stale = self.upload_dir / f"{turn_id - KEEP_UPLOADS:05d}.wav"
            stale.unlink(missing_ok=True)
            self.turns = {k: v for k, v in self.turns.items() if k > turn_id - 10}
            self.turns[turn_id] = {"owner": client, "heard": None, "reply": None, "tools": [],
                                   "started": time.monotonic()}
            self.state = "recognizing"
            self.holder = None
            self._send(f"wav {path}")
            self._changed()
            return turn_id, ""

    def status(self, client, since, timeout):
        with self.cond:
            self.cond.wait_for(lambda: self.seq != since, timeout=timeout)
            holder = self._holder_active()
            turn = self.turns.get(self.next_id)
            view = None
            if turn is not None:
                view = {"id": self.next_id, "mine": turn["owner"] == client, "heard": turn["heard"],
                        "reply": turn["reply"], "tools": turn["tools"], "error": turn.get("error")}
            return {"seq": self.seq, "alive": self.alive, "ready": self.ready, "state": self.state,
                    "holder": None if holder is None else ("me" if holder == client else "other"),
                    "turn": view, "max_hold_s": self.max_hold_s}

    def stop(self):
        if self.proc.poll() is None:  # 已经退出时再写 quit 只会在退出清理时多打一条 BrokenPipeError
            self._send("quit")
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            # 播放设备被拔掉后 ptt_chat 可能卡在退出里，SIGTERM 也不理；不杀掉就成了占着声卡的孤儿
            log("ptt_chat 5 s 内没退出，强制结束")
            self.proc.kill()
            self.proc.wait()


class Server(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 64  # 每个手机常驻一个长轮询


class DualStackServer(Server):
    """监听 [::] 时同时接受 IPv4 和 IPv6：局域网 DNS 会给出机器人的 IPv6 地址，手机可能优先走 IPv6。"""

    address_family = socket.AF_INET6

    def server_bind(self):
        self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        super().server_bind()

    def handle_error(self, request, client_address):
        # 手机切后台、锁屏时长轮询连接会被断开，回包写不出去是正常情况；TLS 握手失败（扫描器、证书不认）
        # 也只是这一个连接的事。都不打印堆栈。
        error = sys.exc_info()[1]
        if isinstance(error, ssl.SSLError):
            # 手机不认证书时这里是 TLSV1_ALERT_UNKNOWN_CA / CERTIFICATE_UNKNOWN，是排查打不开页面的主要线索
            log(f"TLS 握手失败 {client_address[0]}: {getattr(error, 'reason', None) or error}")
            return
        if isinstance(error, (ConnectionError, TimeoutError)):
            return
        super().handle_error(request, client_address)


def make_redirect_handler(target):
    """纯 HTTP 跳转入口：别的页面（如机器人控制台）只知道机器人的 IP，用它链过来，不用知道域名和 token。"""

    class RedirectHandler(BaseHTTPRequestHandler):
        server_version = "ptt_web"
        sys_version = ""
        timeout = 10

        def log_message(self, fmt, *args):
            pass

        def do_GET(self):
            log(f"跳转 {self.client_address[0]} | {self.headers.get('User-Agent', '-')[:160]}")
            self.send_response(302)
            self.send_header("Location", target)
            self.send_header("Content-Length", "0")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Referrer-Policy", "no-referrer")
            self.end_headers()

    return RedirectHandler


def make_server(listen, handler):
    host, port = listen.rsplit(":", 1)
    if host.startswith("[") and host.endswith("]"):
        return DualStackServer((host[1:-1], int(port)), handler)
    return Server((host, int(port)), handler)


def make_handler(robot, token):
    prefix = f"/{token}/"

    class Handler(BaseHTTPRequestHandler):
        server_version = "ptt_web"
        sys_version = ""
        timeout = 60  # 长轮询最多挂 20 s；空连接 60 s 后断开，不占线程

        def setup(self):
            super().setup()  # 先设好超时，握手卡住的连接到点就断
            if isinstance(self.connection, ssl.SSLSocket):
                self.connection.do_handshake()

        def log_message(self, fmt, *args):
            pass

        def _reply(self, code, body, content_type="application/json; charset=utf-8"):
            data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode()
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("Content-Security-Policy", CSP)
            self.end_headers()
            self.wfile.write(data)

        def _route(self):
            url = urlsplit(self.path)
            if url.path == f"/{token}":
                return "redirect", {}
            if not url.path.startswith(prefix):
                return None, {}
            query = {k: v[-1] for k, v in parse_qs(url.query).items()}
            return url.path[len(prefix):], query

        def do_GET(self):
            route, query = self._route()
            if route == "redirect":
                self.send_response(301)
                self.send_header("Location", prefix)
                self.send_header("Content-Length", "0")
                self.end_headers()
            elif route in STATIC:
                name, content_type = STATIC[route]
                if route == "":
                    log(f"打开页面 {self.client_address[0]} | {self.headers.get('User-Agent', '-')[:160]}")
                self._reply(200, (HERE / name).read_bytes(), content_type)
            elif route == "api/status":
                try:
                    since = int(query.get("since", "-1"))
                except ValueError:
                    since = -1
                self._reply(200, robot.status(query.get("client", ""), since, 20))
            else:
                self._reply(404, {"ok": False})

        def do_POST(self):
            route, query = self._route()
            client = query.get("client", "")
            if route not in ("api/press", "api/release", "api/audio") or not 8 <= len(client) <= 64:
                self._reply(404, {"ok": False})
                return
            length = int(self.headers.get("Content-Length") or 0)
            if length > MAX_UPLOAD:
                self._reply(413, {"ok": False, "error": "录音太长"})
                self.close_connection = True
                return
            body = self.rfile.read(length) if length else b""
            peer = self.headers.get("CF-Connecting-IP") or self.headers.get("X-Forwarded-For") or self.client_address[0]
            if route == "api/press":
                ok, error = robot.press(client)
                agent = self.headers.get("User-Agent", "-")[:160]
                log(f"按下 {client[:6]} {peer}: {'ok' if ok else error} | {agent}")
                self._reply(200, {"ok": ok, "error": error})
            elif route == "api/release":
                robot.release(client)
                self._reply(200, {"ok": True})
            else:
                if len(body) < 44 or body[:4] != b"RIFF" or body[8:12] != b"WAVE":
                    robot.release(client)
                    self._reply(400, {"ok": False, "error": "不是 wav 录音"})
                    return
                turn_id, error = robot.submit(client, body)
                seconds = (len(body) - 44) / 32000
                result = f"#{turn_id}" if turn_id else error
                log(f"录音 {client[:6]} {len(body) // 1024} KB {seconds:.1f} s -> {result}")
                self._reply(200, {"ok": turn_id is not None, "id": turn_id, "error": error})

    return Handler


def load_token(path):
    path = Path(path)
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(secrets.token_urlsafe(12) + "\n")
    token = path.read_text().strip()
    if len(token) < 12 or not token.replace("-", "").replace("_", "").isalnum():
        sys.exit(f"token 无效: {path}")
    return token


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--listen", default="127.0.0.1:18080",
                        help="监听地址，[::]:PORT 同时监听 IPv4 和 IPv6 (默认: 127.0.0.1:18080)")
    parser.add_argument("--token-file", default=os.path.expanduser("~/.config/ptt_web/token"),
                        help="URL token，不存在时生成 (默认: ~/.config/ptt_web/token)")
    parser.add_argument("--upload-dir", default=None,
                        help=f"录音存放目录，只留最近 {KEEP_UPLOADS} 段 (默认: $XDG_RUNTIME_DIR/ptt_web)")
    parser.add_argument("--max-hold", type=int, default=15, help="最长按住秒数 (默认: 15)")
    parser.add_argument("--idle-reset", type=int, default=0, help="空闲多少秒后清空对话记忆，0 不清 (默认: 0)")
    parser.add_argument("--chat-log", default=None, help="ptt_chat 的输出写到这个文件 (默认: 和本服务同一个输出)")
    parser.add_argument("--tls-cert", default=None, help="HTTPS 证书链 (PEM)，和 --tls-key 一起给")
    parser.add_argument("--tls-key", default=None, help="HTTPS 私钥 (PEM)")
    parser.add_argument("--redirect-listen", default=None,
                        help="另开一个纯 HTTP 端口，所有 GET 都 302 到 --redirect-to，给别的页面一个不带 token 的入口")
    parser.add_argument("--redirect-to", default=None, help="跳转目标，通常是带 token 的 https 页面地址")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="-- ptt_chat 命令行")
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("缺少 ptt_chat 命令行")
    if bool(args.tls_cert) != bool(args.tls_key):
        parser.error("--tls-cert 和 --tls-key 要一起给")
    if bool(args.redirect_listen) != bool(args.redirect_to):
        parser.error("--redirect-listen 和 --redirect-to 要一起给")
    tls = None
    if args.tls_cert:
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.minimum_version = ssl.TLSVersion.TLSv1_2
        tls.load_cert_chain(args.tls_cert, args.tls_key)  # 证书有问题时在拉起 ptt_chat 之前就报错

    token = load_token(args.token_file)
    upload_dir = Path(args.upload_dir or os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "ptt_web"))
    upload_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    robot = Robot(command, upload_dir, args.max_hold, args.idle_reset, args.chat_log)
    server = make_server(args.listen, make_handler(robot, token))
    redirect = None
    if args.redirect_listen:
        redirect = make_server(args.redirect_listen, make_redirect_handler(args.redirect_to))
        threading.Thread(target=redirect.serve_forever, daemon=True).start()
        log(f"跳转入口 http://{args.redirect_listen}/ -> 页面")
    if tls is not None:
        # 握手推迟到各连接自己的处理线程里第一次读时做，慢客户端不会卡住 accept
        server.socket = tls.wrap_socket(server.socket, server_side=True, do_handshake_on_connect=False)

    def watch():
        robot.proc.wait()
        server.shutdown()

    threading.Thread(target=watch, daemon=True).start()
    signal.signal(signal.SIGTERM, lambda *_: threading.Thread(target=server.shutdown, daemon=True).start())
    log(f"监听 {'https' if tls else 'http'}://{args.listen}，页面路径 /<token>/（token 在 {args.token_file}）")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        robot.stop()
        server.server_close()
        if redirect is not None:
            redirect.shutdown()
            redirect.server_close()
    sys.exit(0 if robot.proc.returncode in (0, None) else 1)


if __name__ == "__main__":
    main()
