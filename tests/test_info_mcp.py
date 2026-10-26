#!/usr/bin/env python3
#
# Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
# SPDX-License-Identifier: Apache-2.0
#
"""tools/info_mcp/info_mcp.py：MCP 握手、工具列表、时间、天气（默认城市和内置城市不查坐标、别的城市查坐标、
失败重试一次、缓存、找不到城市、服务连不上、数据不对），全部对着本地的假 Open-Meteo，不联网。"""

import json
import re
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

SERVER = Path(__file__).resolve().parent.parent / "tools" / "info_mcp" / "info_mcp.py"
FORECAST = {
    "current": {"temperature_2m": 26.4, "apparent_temperature": 28.1, "relative_humidity_2m": 70,
                "weather_code": 2, "wind_speed_10m": 14.0},
    "daily": {"temperature_2m_max": [29.2], "temperature_2m_min": [23.0], "precipitation_probability_max": [20]},
}


class FakeOpenMeteo(BaseHTTPRequestHandler):
    seen = []
    broken = False
    drop_next = 0  # 接下来这么多个请求直接断开（模拟 4G 下握手卡住）

    def do_GET(self):
        url = urlsplit(self.path)
        query = {k: v[0] for k, v in parse_qs(url.query).items()}
        FakeOpenMeteo.seen.append((url.path, query))
        if FakeOpenMeteo.drop_next:
            FakeOpenMeteo.drop_next -= 1
            self.close_connection = True
            return
        if url.path == "/geocode":
            body = (
                {"results": [{"name": "汕尾", "latitude": 22.79, "longitude": 115.37}]}
                if query["name"] == "汕尾" else {}
            )
        elif FakeOpenMeteo.broken:
            body = {"current": {}}
        else:
            body = FORECAST
        data = json.dumps(body, ensure_ascii=False).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


def check(cond, what):
    if not cond:
        raise AssertionError(what)


class Client:
    def __init__(self, port):
        base = f"http://127.0.0.1:{port}"
        self.proc = subprocess.Popen(
            [sys.executable, str(SERVER), "--default-city", "深圳", "--default-location", "22.54,114.06",
             "--weather-url", f"{base}/forecast", "--geocode-url", f"{base}/geocode", "--timeout", "2"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.next_id = 0

    def send(self, method, params=None, notify=False):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        if not notify:
            self.next_id += 1
            message["id"] = self.next_id
        self.proc.stdin.write(json.dumps(message, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()
        if notify:
            return None
        answer = json.loads(self.proc.stdout.readline())
        check(answer.get("id") == self.next_id, f"回复 id 不对: {answer}")
        return answer

    def call(self, name, arguments=None):
        result = self.send("tools/call", {"name": name, "arguments": arguments or {}})["result"]
        return result["content"][0]["text"], result["isError"]


def main():
    fake = ThreadingHTTPServer(("127.0.0.1", 0), FakeOpenMeteo)
    threading.Thread(target=fake.serve_forever, daemon=True).start()
    client = Client(fake.server_address[1])
    try:
        init = client.send("initialize", {"protocolVersion": "2025-03-26"})["result"]
        check(init["protocolVersion"] == "2025-03-26" and "tools" in init["capabilities"], f"initialize: {init}")
        old = client.send("initialize", {"protocolVersion": "1999-01-01"})["result"]
        check(old["protocolVersion"] == "2024-11-05", f"不认识的版本应退回最老的: {old}")
        check(client.send("notifications/initialized", notify=True) is None, "通知不回")
        tools = client.send("tools/list")["result"]["tools"]
        check([t["name"] for t in tools] == ["get_time", "get_weather"], f"tools/list: {tools}")
        check("深圳" in tools[1]["inputSchema"]["properties"]["city"]["description"], "默认城市写进参数说明")

        text, failed = client.call("get_time")
        check(not failed and re.fullmatch(
            r"现在是 \d{4} 年 \d{1,2} 月 \d{1,2} 日，星期[一二三四五六日]，\d\d:\d\d。", text),
              f"get_time: {text}")

        text, failed = client.call("get_weather")
        check(not failed and text == (
            "深圳现在多云，气温 26 度，体感 28 度，湿度 70%，风力 3 级。"
            "今天 23 到 29 度，降水概率 20%。"),
              f"默认城市天气: {text}")
        check([p for p, _ in FakeOpenMeteo.seen] == ["/forecast"], f"默认城市不查坐标: {FakeOpenMeteo.seen}")
        query = FakeOpenMeteo.seen[0][1]
        check(query["latitude"] == "22.54" and query["timezone"] == "Asia/Shanghai", f"天气请求参数: {query}")
        check(client.call("get_weather", {"city": "深圳"})[0].startswith("深圳现在多云")
              and len(FakeOpenMeteo.seen) == 1,
              f"10 分钟内同一地点用缓存: {FakeOpenMeteo.seen}")

        text, failed = client.call("get_weather", {"city": "北京市"})
        check(not failed and text.startswith("北京现在多云"), f"内置城市: {text}")
        check([p for p, _ in FakeOpenMeteo.seen[1:]] == ["/forecast"]
              and FakeOpenMeteo.seen[1][1]["latitude"] == "39.9",
              f"内置城市不查坐标: {FakeOpenMeteo.seen}")

        FakeOpenMeteo.seen.clear()
        FakeOpenMeteo.drop_next = 1
        text, failed = client.call("get_weather", {"city": "汕尾"})
        check(not failed and text.startswith("汕尾现在多云"), f"别的城市: {text}")
        check([p for p, _ in FakeOpenMeteo.seen] == ["/geocode", "/geocode", "/forecast"],
              f"第一次断开后重试，再按查到的坐标查天气: {FakeOpenMeteo.seen}")
        check(FakeOpenMeteo.seen[2][1]["latitude"] == "22.79", "用查到的坐标")

        text, failed = client.call("get_weather", {"city": "不存在市"})
        check(failed and "没找到" in text, f"找不到城市: {text}")

        FakeOpenMeteo.broken = True
        FakeOpenMeteo.seen.clear()
        text, failed = client.call("get_weather", {"city": "汕尾"})
        check(not failed and not FakeOpenMeteo.seen, f"缓存命中不发请求，服务坏了也不受影响: {text}")

        error = client.send("tools/call", {"name": "nope", "arguments": {}})
        check(error["error"]["code"] == -32602, f"未知工具: {error}")
        check(client.send("nope")["error"]["code"] == -32601, "未知方法")

        fake.shutdown()
        fake.server_close()
        client2 = Client(fake.server_address[1])
        try:
            client2.send("initialize", {})
            text, failed = client2.call("get_weather")
            check(failed and "连不上" in text, f"服务连不上: {text}")
        finally:
            client2.proc.kill()
    finally:
        client.proc.kill()

    broken = ThreadingHTTPServer(("127.0.0.1", 0), FakeOpenMeteo)
    threading.Thread(target=broken.serve_forever, daemon=True).start()
    client3 = Client(broken.server_address[1])
    try:
        client3.send("initialize", {})
        text, failed = client3.call("get_weather")
        check(failed and "看不懂" in text, f"数据不对: {text}")
    finally:
        client3.proc.kill()
        broken.shutdown()
    print("info_mcp: all checks passed")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        print(f"FAIL: {error}")
        sys.exit(1)
