"""免费天气插件（Open-Meteo，无需 API Key）

挂载到 /opt/xiaozhi-esp32-server/plugins_func/functions/ 后由 loadplugins 自动加载。
"""

import httpx
from typing import TYPE_CHECKING

from config.logger import setup_logging
from core.utils.util import get_ip_info
from plugins_func.register import register_function, ToolType, ActionResponse, Action

if TYPE_CHECKING:
    from core.connection import ConnectionHandler

TAG = __name__
logger = setup_logging()

WEATHER_NOW_DESC = {
    "type": "function",
    "function": {
        "name": "get_weather_now",
        "description": (
            "查询某城市当前天气和今明两天的温度。用户问天气时调用。"
            "不指定城市时用设备所在城市。"
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "location": {
                    "type": "string",
                    "description": "城市名，例如 北京、杭州、广州。用户没说城市就不要传",
                },
            },
            "required": [],
        },
    },
}

# WMO 天气码 -> 中文
WMO = {
    0: "晴", 1: "晴间多云", 2: "多云", 3: "阴",
    45: "有雾", 48: "雾凇",
    51: "小毛毛雨", 53: "毛毛雨", 55: "大毛毛雨",
    56: "冻毛毛雨", 57: "强冻毛毛雨",
    61: "小雨", 63: "中雨", 65: "大雨",
    66: "冻雨", 67: "强冻雨",
    71: "小雪", 73: "中雪", 75: "大雪", 77: "雪粒",
    80: "阵雨", 81: "强阵雨", 82: "暴雨",
    85: "阵雪", 86: "强阵雪",
    95: "雷阵雨", 96: "雷阵雨伴冰雹", 99: "强雷阵雨伴冰雹",
}


def _wmo(code) -> str:
    return WMO.get(int(code), f"天气码{code}")


async def _geocode(name: str):
    url = "https://geocoding-api.open-meteo.com/v1/search"
    params = {"name": name, "count": 1, "language": "zh", "format": "json"}
    async with httpx.AsyncClient(timeout=httpx.Timeout(8.0, connect=4.0)) as c:
        r = await c.get(url, params=params)
        if r.status_code != 200:
            return None
        data = r.json()
    results = data.get("results") or []
    if not results:
        return None
    g = results[0]
    return {
        "name": g.get("name") or name,
        "admin": g.get("admin1") or "",
        "lat": g["latitude"],
        "lon": g["longitude"],
    }


async def _device_city(conn):
    """用设备公网 IP 反查城市，失败就用北京。"""
    ip_addr = getattr(conn, "client_ip", None) if conn is not None else None
    if ip_addr:
        try:
            # 注意：get_ip_info 需要 (ip, logger) 两个参数
            info = get_ip_info(ip_addr, logger)
            city = (info or {}).get("city")
            if city:
                return city
        except Exception as exc:  # noqa: BLE001
            logger.bind(tag=TAG).warning(f"IP 定位失败: {exc}")
    return "北京"


def _strip_suffix(name: str) -> str:
    """「无锡市」→「无锡」，应对地理编码对后缀敏感的情况。"""
    for suffix in ("特别行政区", "自治区", "自治州", "地区", "新区", "市辖区", "市", "区", "县"):
        if name.endswith(suffix) and len(name) > len(suffix) + 1:
            return name[: -len(suffix)]
    return name


@register_function("get_weather_now", WEATHER_NOW_DESC, ToolType.SYSTEM_CTL)
async def get_weather_now(conn=None, location: str = ""):
    # SYSTEM_CTL 类型：执行器会以 func(conn, **schema参数) 的方式调用，
    # 这样才拿得到 conn.client_ip 做 IP 定位（WAIT 类型不传 conn）。
    # conn 仍然给默认值，兼容直接调用。
    city_name = (location or "").strip()
    if not city_name:
        city_name = await _device_city(conn)

    try:
        geo = await _geocode(city_name)
        if not geo:
            # 带后缀查不到时，去掉「市/区/县」再试一次
            alt = _strip_suffix(city_name)
            if alt != city_name:
                geo = await _geocode(alt)
    except Exception as exc:  # noqa: BLE001
        logger.bind(tag=TAG).error(f"地理编码失败 {city_name}: {exc}")
        return ActionResponse(action=Action.RESPONSE, response="我这边查天气出了点问题，稍后再试吧。")

    if not geo:
        return ActionResponse(
            action=Action.RESPONSE, response=f"没找到「{city_name}」这个地方，换个城市名试试？"
        )

    url = "https://api.open-meteo.com/v1/forecast"
    params = {
        "latitude": geo["lat"],
        "longitude": geo["lon"],
        "current": "temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m",
        "daily": "temperature_2m_max,temperature_2m_min,weather_code",
        "timezone": "Asia/Shanghai",
        "forecast_days": 2,
    }
    try:
        async with httpx.AsyncClient(timeout=httpx.Timeout(10.0, connect=4.0)) as c:
            r = await c.get(url, params=params)
            if r.status_code != 200:
                raise RuntimeError(f"HTTP {r.status_code}")
            d = r.json()
    except Exception as exc:  # noqa: BLE001
        logger.bind(tag=TAG).error(f"天气请求失败: {exc}")
        return ActionResponse(action=Action.RESPONSE, response="天气服务暂时连不上，等下再问我吧。")

    cur = d.get("current") or {}
    daily = d.get("daily") or {}
    admin = (geo["admin"] or "").strip()
    name = (geo["name"] or "").strip()
    if not admin:
        place = name
    elif name and name in admin:
        place = admin
    elif admin in name:
        place = name
    else:
        place = f"{admin}{name}"

    t = cur.get("temperature_2m")
    feels = cur.get("apparent_temperature")
    hum = cur.get("relative_humidity_2m")
    wind = cur.get("wind_speed_10m")
    desc = _wmo(cur.get("weather_code", -1))

    parts = [f"{place}现在{desc}，气温 {t}°C"]
    if feels is not None and abs((feels or 0) - (t or 0)) >= 2:
        parts.append(f"体感 {feels}°C")
    if hum is not None:
        parts.append(f"湿度 {hum}%")
    if wind is not None:
        parts.append(f"风速 {wind} km/h")

    line = "，".join(parts) + "。"

    try:
        hi = daily["temperature_2m_max"]
        lo = daily["temperature_2m_min"]
        codes = daily["weather_code"]
        line += f"今天 {lo[0]}~{hi[0]}°C，{_wmo(codes[0])}；明天 {lo[1]}~{hi[1]}°C，{_wmo(codes[1])}。"
    except Exception:  # noqa: BLE001
        pass

    logger.bind(tag=TAG).info(f"天气查询 {place}: {line}")
    return ActionResponse(action=Action.RESPONSE, response=line)
