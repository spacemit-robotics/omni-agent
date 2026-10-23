#!/usr/bin/env python3
#
# Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
# SPDX-License-Identifier: Apache-2.0
#
"""给语音助手查时间和天气的 MCP 服务（stdio，只依赖 Python 标准库）。

    info_mcp.py [--default-city 深圳 --default-location 22.5431,114.0579] [--timezone Asia/Shanghai]

工具：
  get_time      现在的日期、星期和时间（本机时钟，--timezone 时区）
  get_weather   某个城市现在的天气和今天的气温范围；不给城市就查 --default-city。数据来自 Open-Meteo（免费、不要密钥）。
                默认城市用 --default-location，国内主要城市用内置坐标，其余经 Open-Meteo 地理编码查坐标。
                机器人走 4G 访问境外服务器时偶尔握手卡住，每个请求失败后重试一次；同一地点 10 分钟内复用结果。

mcp.json 里这样接：
{"name": "Info", "type": "stdio", "command": "python3", "args": ["<本文件>", "--default-city", "深圳", ...]}
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from zoneinfo import ZoneInfo

PROTOCOL_VERSIONS = ("2025-06-18", "2025-03-26", "2024-11-05")
WEATHER_URL = "https://api.open-meteo.com/v1/forecast"
GEOCODE_URL = "https://geocoding-api.open-meteo.com/v1/search"
CACHE_S = 600
WEEKDAYS = "一二三四五六日"
# WMO 天气代码（Open-Meteo weather_code）
WEATHER_CODES = {
    0: "晴", 1: "晴间多云", 2: "多云", 3: "阴", 45: "有雾", 48: "有雾凇",
    51: "小毛毛雨", 53: "毛毛雨", 55: "大毛毛雨", 56: "冻毛毛雨", 57: "冻毛毛雨",
    61: "小雨", 63: "中雨", 65: "大雨", 66: "冻雨", 67: "冻雨",
    71: "小雪", 73: "中雪", 75: "大雪", 77: "米雪",
    80: "阵雨", 81: "强阵雨", 82: "暴雨", 85: "阵雪", 86: "强阵雪",
    95: "雷阵雨", 96: "雷阵雨伴有冰雹", 99: "强雷阵雨伴有冰雹",
}
# 国内主要城市的坐标（纬度, 经度），常问的城市不用再查地理编码
CITIES = {
    "北京": (39.90, 116.41), "上海": (31.23, 121.47), "广州": (23.13, 113.26), "深圳": (22.54, 114.06),
    "天津": (39.13, 117.20), "重庆": (29.56, 106.55), "杭州": (30.27, 120.16), "南京": (32.06, 118.80),
    "苏州": (31.30, 120.59), "无锡": (31.49, 120.31), "宁波": (29.87, 121.54), "成都": (30.57, 104.07),
    "武汉": (30.59, 114.31), "西安": (34.34, 108.94), "长沙": (28.23, 112.94), "郑州": (34.75, 113.63),
    "青岛": (36.07, 120.38), "济南": (36.65, 117.12), "合肥": (31.82, 117.23), "厦门": (24.48, 118.09),
    "福州": (26.07, 119.30), "南昌": (28.68, 115.86), "沈阳": (41.81, 123.43), "大连": (38.91, 121.61),
    "哈尔滨": (45.80, 126.53), "长春": (43.82, 125.32), "石家庄": (38.04, 114.51), "太原": (37.87, 112.55),
    "呼和浩特": (40.84, 111.75), "昆明": (25.04, 102.71), "贵阳": (26.65, 106.63), "南宁": (22.82, 108.37),
    "海口": (20.04, 110.20), "三亚": (18.25, 109.51), "拉萨": (29.65, 91.14), "乌鲁木齐": (43.83, 87.62),
    "兰州": (36.06, 103.83), "西宁": (36.62, 101.78), "银川": (38.49, 106.23), "珠海": (22.27, 113.58),
    "东莞": (23.02, 113.75), "佛山": (23.02, 113.12), "香港": (22.32, 114.17), "澳门": (22.20, 113.55),
    "台北": (25.03, 121.57),
}
# 风速（km/h）上限对应的风力等级
BEAUFORT_KMH = (1, 6, 12, 20, 29, 39, 50, 62, 75, 89, 103, 118)


def log(message):
    print(f"[info_mcp] {message}", file=sys.stderr, flush=True)


class ToolError(Exception):
    """给模型看的失败原因（中文，tools/call 里 isError=true）。"""


class Info:
    def __init__(self, args):
        self.timezone = ZoneInfo(args.timezone)
        self.default_city = args.default_city
        self.default_location = None
        if args.default_location:
            lat, lon = (float(part) for part in args.default_location.split(","))
            self.default_location = (args.default_city, lat, lon)
        self.weather_url = args.weather_url
        self.geocode_url = args.geocode_url
        self.timeout = args.timeout
        self.places = {}   # 城市名 -> (显示名, 纬度, 经度)
        self.weather = {}  # (纬度, 经度) -> (时间, 结果)

    def tools(self):
        city = f"不给就查默认城市{self.default_city}" if self.default_city else "必填"
        return [
            {"name": "get_time", "description": "查现在的日期、星期几和时间。用户问今天几号、星期几、现在几点时调用。",
             "inputSchema": {"type": "object", "properties": {}}},
            {"name": "get_weather",
             "description": "查一个城市现在的天气和今天的气温范围。用户问天气、冷不冷、要不要带伞时调用。",
             "inputSchema": {"type": "object", "properties": {
                 "city": {"type": "string", "description": f"城市名，例如 北京、上海；{city}"}}}},
        ]

    def call(self, name, arguments):
        if name == "get_time":
            now = datetime.now(self.timezone)
            return f"现在是 {now.year} 年 {now.month} 月 {now.day} 日，星期{WEEKDAYS[now.weekday()]}，{now:%H:%M}。"
        return self.get_weather(str(arguments.get("city") or "").strip())

    def fetch(self, url, params):
        request = urllib.request.Request(f"{url}?{urllib.parse.urlencode(params)}", headers={"User-Agent": "info_mcp"})
        for attempt in (1, 2):
            try:
                with urllib.request.urlopen(request, timeout=self.timeout) as response:
                    return json.load(response)
            except (urllib.error.URLError, TimeoutError, OSError, ValueError) as error:
                log(f"请求失败（第 {attempt} 次）{url}: {error}")
                if attempt == 2:
                    raise ToolError("天气服务暂时连不上，查不到天气。") from error
        raise AssertionError("unreachable")

    def locate(self, city):
        if not city or city == self.default_city:
            if self.default_location:
                return self.default_location
            if not city:
                raise ToolError("没有说要查哪个城市。")
        city = city.removesuffix("市") if len(city) > 2 else city
        if city in CITIES:
            return (city, *CITIES[city])
        if city in self.places:
            return self.places[city]
        found = self.fetch(self.geocode_url, {"name": city, "count": 1, "language": "zh", "format": "json"})
        results = found.get("results") or []
        if not results:
            raise ToolError(f"没找到叫「{city}」的地方。")
        place = results[0]
        try:
            located = (place.get("name") or city, float(place["latitude"]), float(place["longitude"]))
        except (KeyError, TypeError, ValueError) as error:
            log(f"地理编码数据格式不对: {error}")
            raise ToolError(f"查不到「{city}」的位置。") from error
        self.places[city] = located
        return located

    def get_weather(self, city):
        name, lat, lon = self.locate(city)
        key = (round(lat, 3), round(lon, 3))
        cached = self.weather.get(key)
        if cached and time.monotonic() - cached[0] < CACHE_S:
            data = cached[1]
        else:
            data = self.fetch(self.weather_url, {
                "latitude": lat, "longitude": lon, "timezone": self.timezone.key, "forecast_days": 1,
                "current": "temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m",
                "daily": "temperature_2m_max,temperature_2m_min,precipitation_probability_max",
            })
            self.weather[key] = (time.monotonic(), data)
        try:
            current, daily = data["current"], data["daily"]
            text = (f"{name}现在{WEATHER_CODES.get(int(current['weather_code']), '天气未知')}，"
                    f"气温 {current['temperature_2m']:.0f} 度，体感 {current['apparent_temperature']:.0f} 度，"
                    f"湿度 {current['relative_humidity_2m']:.0f}%，风力 {beaufort(current['wind_speed_10m'])} 级。"
                    f"今天 {daily['temperature_2m_min'][0]:.0f} 到 {daily['temperature_2m_max'][0]:.0f} 度")
            rain = daily.get("precipitation_probability_max", [None])[0]
            return text + (f"，降水概率 {rain:.0f}%。" if rain is not None else "。")
        except (KeyError, IndexError, TypeError, ValueError) as error:
            log(f"天气数据格式不对: {error}")
            raise ToolError("天气服务返回的数据看不懂，查不到天气。") from error


def beaufort(kmh):
    for level, limit in enumerate(BEAUFORT_KMH):
        if kmh < limit:
            return level
    return 12


def handle(info, message):
    """处理一条 JSON-RPC 消息，返回要回的消息（通知不回）。"""
    if not isinstance(message, dict) or not isinstance(message.get("method"), str):
        if isinstance(message, dict) and "id" in message and "result" not in message and "error" not in message:
            return error_reply(message["id"], -32600, "invalid request")
        return None
    method, msg_id = message["method"], message.get("id")
    if msg_id is None:
        return None
    params = message.get("params") if isinstance(message.get("params"), dict) else {}
    if method == "initialize":
        requested = params.get("protocolVersion")
        version = requested if requested in PROTOCOL_VERSIONS else PROTOCOL_VERSIONS[-1]
        return reply(msg_id, {"protocolVersion": version, "capabilities": {"tools": {}},
                              "serverInfo": {"name": "info_mcp", "version": "1.0"}})
    if method == "ping":
        return reply(msg_id, {})
    if method == "tools/list":
        return reply(msg_id, {"tools": info.tools()})
    if method == "tools/call":
        name = params.get("name")
        if not isinstance(name, str):
            return error_reply(msg_id, -32602, "params.name must be a string")
        if name not in ("get_time", "get_weather"):
            return error_reply(msg_id, -32602, f"unknown tool: {name}")
        arguments = params.get("arguments") if isinstance(params.get("arguments"), dict) else {}
        try:
            text, failed = info.call(name, arguments), False
        except ToolError as error:
            text, failed = str(error), True
        log(f"{name} {json.dumps(arguments, ensure_ascii=False)} -> {text}")
        return reply(msg_id, {"content": [{"type": "text", "text": text}], "isError": failed})
    return error_reply(msg_id, -32601, f"method not found: {method}")


def reply(msg_id, result):
    return {"jsonrpc": "2.0", "id": msg_id, "result": result}


def error_reply(msg_id, code, message):
    return {"jsonrpc": "2.0", "id": msg_id, "error": {"code": code, "message": message}}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--default-city", default="", help="没说城市时查哪里，例如 深圳")
    parser.add_argument("--default-location", default="", help="默认城市的 纬度,经度，给了就不用查坐标")
    parser.add_argument("--timezone", default="Asia/Shanghai")
    parser.add_argument("--timeout", type=float, default=5, help="每次请求的超时秒数，失败重试一次 (默认: 5)")
    parser.add_argument("--weather-url", default=WEATHER_URL)
    parser.add_argument("--geocode-url", default=GEOCODE_URL)
    info = Info(parser.parse_args())
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except ValueError:
            answer = error_reply(None, -32700, "parse error")
        else:
            answer = handle(info, message)
        if answer is not None:
            sys.stdout.write(json.dumps(answer, ensure_ascii=False) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
