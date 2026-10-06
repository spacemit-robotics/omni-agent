#!/usr/bin/env python3
#
# Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
# SPDX-License-Identifier: Apache-2.0
#
"""机器人控制台的语音对话：给控制台网页提供 HTTPS 和按住说话的语音面板，录音交给 ptt_chat 识别、回答并用本机扬声器播放。

    ptt_server.py --operator-connection FILE [--console 127.0.0.1:8765] [--listen 127.0.0.1:18080]
                  [--tls-cert C --tls-key K] [--redirect-listen [::]:8442 --redirect-to URL]
                  [--actions k=名,...] -- ptt_chat ...

手机浏览器只在 HTTPS 页面里允许录音，而机器人控制台（humanoid_common operator_service）是纯 HTTP，所以由本服务
给它套一层 HTTPS：/voice/ 以外的请求原样转发给 --console（含 WebSocket），控制台网页再从 /voice/panel.js 加载语音面板，
在同一页里切换「控制 | 语音」。语音接口 /voice/api/* 要带控制台的配对令牌（X-Operator-Token，和 --operator-connection
连接文件里的一致），没配对的人用不了。

一次只让一个人说；机器人回答某个人时，别人要等它说完。面板显示最近的对话（所有人的），回答边生成边显示；
动作清单来自 --actions（和 humanoid_operator_mcp 同一格式）。
"""

import argparse
import hmac
import json
import os
import select
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
VOICE = "/voice/"
STATIC = {"panel.js": ("panel.js", "text/javascript; charset=utf-8"), "panel.css": ("panel.css", "text/css; charset=utf-8")}
# 控制台只给本机用的入口：对端是本机且 Host 是 localhost 时直接发配对令牌（server.cpp）。经本服务转发时对端就是本机，
# Host 又由请求方决定，转发出去等于把令牌发给局域网里任何人，所以一律不转发。
BLOCKED = ("/api/v1/local-session", "/pairing.svg")
TUNNEL_IDLE_S = 120           # 转发的连接两头都这么久没数据就断开（控制台的 WebSocket 一直有状态推送）
MAX_UPLOAD = 2 * 1024 * 1024  # 16 kHz 单声道 16 位约 60 s
KEEP_UPLOADS = 20
KEEP_TURNS = 30               # 页面上能往回翻的轮数


def log(message):
    print(time.strftime("%H:%M:%S") + " [ptt_web] " + message, flush=True)


class Robot:
    """ptt_chat 子进程和对话状态。所有状态由 cond 保护，seq 每次变化加一，供长轮询等待。"""

    def __init__(self, command, upload_dir, max_hold_s, idle_reset_s, chat_log, actions=None):
        self.actions = actions or {}  # 动作键 -> 显示名
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
        self.turns = {}             # id -> {owner, heard, live, reply, pending, tools, started, wall}
        self.resets = []            # 空闲清空记忆时最后一轮的编号，页面在它后面画分隔线
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
                    if event.get("state") == "idle" and turn is not None:
                        turn["pending"] = []
                elif kind == "asr" and turn is not None:
                    turn["heard"] = event.get("text", "")
                    log(f"#{event['id']} 识别 {event.get('ms')} ms: {turn['heard']!r}")
                elif kind == "delta" and turn is not None:
                    turn["live"] += event.get("text", "")
                elif kind == "tool" and turn is not None:
                    turn["pending"].append(self._tool_label(event.get("name", ""), event.get("args")))
                elif kind == "tool_result" and turn is not None:
                    if turn["pending"]:
                        turn["pending"].pop(0)
                    turn["tools"].append(event.get("text", ""))
                elif kind == "reply" and turn is not None:
                    turn["reply"] = event.get("text", "")
                    turn["tools"] = event.get("tools", [])
                    turn["pending"] = []
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
            time.sleep(min(5, self.idle_reset_s))
            with self.cond:
                idle = self.state == "idle" and self.holder is None
                quiet = time.monotonic() - self.last_activity > self.idle_reset_s
                if not (idle and quiet):
                    reset_done = False
                elif not reset_done:
                    self._send("reset")
                    reset_done = True
                    if self.next_id and self.next_id not in self.resets:
                        self.resets = self.resets[-9:] + [self.next_id]
                        self._changed()
                    log(f"空闲 {self.idle_reset_s} s，清空对话记忆")

    def _tool_label(self, name, args):
        if name == "perform_action" and isinstance(args, dict):
            action = str(args.get("action", ""))
            return f"准备做「{self.actions.get(action, action)}」"
        if name == "stop_action":
            return "准备停止动作"
        return f"准备调用 {name}"

    def _holder_active(self):
        if self.holder is not None and time.monotonic() > self.hold_until:
            log(f"{self.holder} 按住超时，释放")
            self.holder = None
        return self.holder

    def _answering_other(self, client):
        """机器人正在回答别人（不让打断）；卡住超过 60 s 的不算，免得一直按不了。"""
        turn = self.turns.get(self.next_id)
        return (self.state != "idle" and turn is not None and turn["owner"] != client
                and time.monotonic() - turn["started"] < 60)

    def press(self, client):
        with self.cond:
            if not self.ready:
                return False, "机器人还没准备好，请稍等"
            holder = self._holder_active()
            if holder not in (None, client):
                return False, "别人正在说话，请稍等"
            if self._answering_other(client):
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
            self.turns = {k: v for k, v in self.turns.items() if k > turn_id - KEEP_TURNS}
            self.turns[turn_id] = {"owner": client, "heard": None, "live": "", "reply": None, "pending": [],
                                   "tools": [], "started": time.monotonic(), "wall": time.time()}
            self.state = "recognizing"
            self.holder = None
            self._send(f"wav {path}")
            self._changed()
            return turn_id, ""

    def status(self, client, since, timeout):
        with self.cond:
            changed = self.cond.wait_for(lambda: self.seq != since, timeout=timeout)
        if changed and since >= 0:
            time.sleep(0.06)  # 回答边生成边推送，一秒几十段：攒一下再回，手机少折腾
        with self.cond:
            holder = self._holder_active()
            now = time.time()
            history = [{"id": k, "mine": t["owner"] == client, "heard": t["heard"], "live": t["live"],
                        "reply": t["reply"], "pending": list(t["pending"]), "tools": list(t["tools"]),
                        "error": t.get("error"), "interrupted": t.get("interrupted", False),
                        "ago_s": int(now - t["wall"])}
                       for k, t in sorted(self.turns.items())]
            return {"seq": self.seq, "alive": self.alive, "ready": self.ready, "state": self.state,
                    "holder": None if holder is None else ("me" if holder == client else "other"),
                    "answering_other": self._answering_other(client),
                    "live_id": self.next_id, "history": history,
                    "resets": [r for r in self.resets if r in self.turns], "max_hold_s": self.max_hold_s}

    def info(self):
        return {"actions": list(self.actions.values())}

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


class OperatorToken:
    """控制台的配对令牌，取自操作服务写的连接文件（和 humanoid_operator_mcp 读的同一个）；重新配对后文件变了就重读。"""

    def __init__(self, path):
        self.path = Path(path)
        self.lock = threading.Lock()
        self.mtime = None
        self.token = ""

    def check(self, presented):
        with self.lock:
            try:
                mtime = self.path.stat().st_mtime_ns
                if mtime != self.mtime:
                    self.token = str(json.loads(self.path.read_text()).get("token", ""))
                    self.mtime = mtime
            except (OSError, ValueError, AttributeError):
                self.token, self.mtime = "", None
            token = self.token
        return bool(token and presented) and hmac.compare_digest(token.encode(), presented.encode())


def tunnel(client, upstream):
    """两头双向转发，直到任一头关闭或都闲置 TUNNEL_IDLE_S。单线程 select：TLS 连接不能两个线程同时读写。"""
    for sock in (client, upstream):
        sock.settimeout(30)  # 只防 sendall 卡死：读之前 select 已确认有数据
    try:
        while True:
            pending = getattr(client, "pending", None)  # TLS 已解密、还在缓冲里的数据 select 看不到
            ready = [client] if pending and pending() else select.select([client, upstream], [], [], TUNNEL_IDLE_S)[0]
            if not ready:
                return
            for sock in ready:
                data = sock.recv(65536)
                if not data:
                    return
                (upstream if sock is client else client).sendall(data)
    except (OSError, ValueError):
        return


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
    """纯 HTTP 跳转入口：控制台的 HTTP 页面只知道机器人的 IP，经这里跳到本服务的 HTTPS 地址。
    查询参数原样带上；#token= 片段不发给服务端，浏览器跳转后自己保留。"""

    class RedirectHandler(BaseHTTPRequestHandler):
        server_version = "ptt_web"
        sys_version = ""
        timeout = 10

        def log_message(self, fmt, *args):
            pass

        def do_GET(self):
            log(f"跳转 {self.client_address[0]} | {self.headers.get('User-Agent', '-')[:160]}")
            query = urlsplit(self.path).query  # 例如 back=<控制台地址>，页面据此显示「控制模式」
            self.send_response(302)
            self.send_header("Location", target + ("?" + query if query else ""))
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


def make_handler(robot, operator_token, console):
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
            self.end_headers()
            self.wfile.write(data)

        def _voice(self):
            """/voice/ 下的路径，返回 (相对路径, 查询参数)；不是语音的请求返回 (None, {})。"""
            url = urlsplit(self.path)
            if not url.path.startswith(VOICE):
                return None, {}
            return url.path[len(VOICE):], {k: v[-1] for k, v in parse_qs(url.query).items()}

        def _authorized(self):
            if operator_token.check(self.headers.get("X-Operator-Token", "")):
                return True
            self._reply(401, {"ok": False, "error": "请先扫码配对机器人"})
            return False

        def _proxy(self):
            """原样转发给控制台，包括 WebSocket 升级：请求头一字不改（Host / Origin 要和浏览器看到的一致，
            控制台据此做跨站检查），之后两头直接对接。控制台每个 HTTP 响应后都关连接，一次转发只有一个请求。"""
            target = self.path.split("?", 1)[0]
            if console is None or target in BLOCKED or urlsplit(self.path).path in BLOCKED:
                self._reply(404, {"ok": False})
                return
            if self.command == "GET" and target == "/":
                log(f"打开控制台 {self.client_address[0]} | {self.headers.get('User-Agent', '-')[:160]}")
            try:
                upstream = socket.create_connection(console, timeout=5)
            except OSError as error:
                log(f"连不上控制台 {console[0]}:{console[1]}: {error}")
                self._reply(502, {"ok": False, "error": "机器人控制台没有响应"})
                return
            self.close_connection = True
            with upstream:
                lines = [self.requestline] + [f"{key}: {value}" for key, value in self.headers.items()]
                try:
                    upstream.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1"))
                    length = int(self.headers.get("Content-Length") or 0)
                    if 0 < length <= MAX_UPLOAD:
                        upstream.sendall(self.rfile.read(length))
                except (OSError, ValueError):
                    return
                tunnel(self.connection, upstream)

        def do_HEAD(self):
            self._proxy()

        def do_GET(self):
            route, query = self._voice()
            if route is None:
                self._proxy()
            elif route in STATIC:
                name, content_type = STATIC[route]
                self._reply(200, (HERE / name).read_bytes(), content_type)
            elif route == "api/info":
                if self._authorized():
                    self._reply(200, robot.info())
            elif route == "api/status":
                if not self._authorized():
                    return
                try:
                    since = int(query.get("since", "-1"))
                except ValueError:
                    since = -1
                self._reply(200, robot.status(query.get("client", ""), since, 20))
            else:
                self._reply(404, {"ok": False})

        def do_POST(self):
            route, query = self._voice()
            if route is None:
                self._proxy()
                return
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
            if not self._authorized():
                return
            peer = self.client_address[0]
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


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--listen", default="127.0.0.1:18080",
                        help="监听地址，[::]:PORT 同时监听 IPv4 和 IPv6 (默认: 127.0.0.1:18080)")
    parser.add_argument("--operator-connection", required=True,
                        help="操作服务的连接文件（含配对令牌），语音接口用它校验；例如 "
                             "~/.local/state/humanoid-operator/linglong/connection.json")
    parser.add_argument("--console", default=None,
                        help="机器人控制台 HOST:PORT，/voice/ 以外的请求转发过去，例如 127.0.0.1:8765 (默认: 不转发)")
    parser.add_argument("--upload-dir", default=None,
                        help=f"录音存放目录，只留最近 {KEEP_UPLOADS} 段 (默认: $XDG_RUNTIME_DIR/ptt_web)")
    parser.add_argument("--max-hold", type=int, default=15, help="最长按住秒数 (默认: 15)")
    parser.add_argument("--idle-reset", type=int, default=0, help="空闲多少秒后清空对话记忆，0 不清 (默认: 0)")
    parser.add_argument("--chat-log", default=None, help="ptt_chat 的输出写到这个文件 (默认: 和本服务同一个输出)")
    parser.add_argument("--tls-cert", default=None, help="HTTPS 证书链 (PEM)，和 --tls-key 一起给")
    parser.add_argument("--tls-key", default=None, help="HTTPS 私钥 (PEM)")
    parser.add_argument("--redirect-listen", default=None,
                        help="另开一个纯 HTTP 端口，所有 GET 都 302 到 --redirect-to（保留查询参数），给 HTTP 控制台当跳板")
    parser.add_argument("--redirect-to", default=None, help="跳转目标，本服务的 https 地址，如 https://<域名>:8443/")
    parser.add_argument("--actions", default="",
                        help="语音面板上列出的动作，key=显示名,...，和 humanoid_operator_mcp --actions 同一格式")
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

    console = None
    if args.console:
        host, _, port = args.console.rpartition(":")
        if not host or not port.isdigit():
            parser.error("--console 要写成 HOST:PORT")
        console = (host.strip("[]"), int(port))
    upload_dir = Path(args.upload_dir or os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "ptt_web"))
    upload_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    actions = dict(item.split("=", 1) if "=" in item else (item, item)
                   for item in (part.strip() for part in args.actions.split(",")) if item)
    robot = Robot(command, upload_dir, args.max_hold, args.idle_reset, args.chat_log, actions)
    server = make_server(args.listen, make_handler(robot, OperatorToken(args.operator_connection), console))
    redirect = None
    if args.redirect_listen:
        redirect = make_server(args.redirect_listen, make_redirect_handler(args.redirect_to))
        threading.Thread(target=redirect.serve_forever, daemon=True).start()
        log(f"跳转入口 http://{args.redirect_listen}/ -> {args.redirect_to}")
    if tls is not None:
        # 握手推迟到各连接自己的处理线程里第一次读时做，慢客户端不会卡住 accept
        server.socket = tls.wrap_socket(server.socket, server_side=True, do_handshake_on_connect=False)

    def watch():
        robot.proc.wait()
        server.shutdown()

    threading.Thread(target=watch, daemon=True).start()
    signal.signal(signal.SIGTERM, lambda *_: threading.Thread(target=server.shutdown, daemon=True).start())
    target = f"控制台 {args.console}" if console else "不转发控制台"
    log(f"监听 {'https' if tls else 'http'}://{args.listen}，语音面板 {VOICE}panel.js，{target}")
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
