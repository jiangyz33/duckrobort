"""内置 get_weather 的替代实现（免 API Key，走 Open-Meteo）

为什么要覆盖它：
1. 官方 get_weather 依赖和风天气，代码里硬编码的 key 已失效，每次会话启动
   在 prompt_manager 里预取天气都会报 `HTTP 401`；
2. 预取失败后，注入给模型的上下文是「天气信息获取失败」，模型于是跟用户说
   「查不到某某城市的天气」——即使真正调用 get_weather_now 是能查到的。

这个 shim 把预取重定向到 weather_now.py 的 Open-Meteo 实现，
既消掉 401 噪音，又让模型在会话开始时就拿到真实天气。

挂载位置：
  ./plugins/get_weather.py:/opt/xiaozhi-esp32-server/plugins_func/functions/get_weather.py:ro
"""

from plugins_func.register import ActionResponse, Action
from plugins_func.functions.weather_now import get_weather_now


async def get_weather(conn=None, location: str = "", lang: str = "zh_CN"):
    """签名与官方 get_weather 保持一致，内部改为免 key 实现。"""
    try:
        resp = await get_weather_now(conn, location=location or "")
    except Exception as exc:  # noqa: BLE001
        return ActionResponse(action=Action.RESPONSE, result=f"天气查询失败：{exc}")

    # prompt_manager 读的是 .result，而 weather_now 填的是 .response
    text = getattr(resp, "response", None) or getattr(resp, "result", None) or ""
    return ActionResponse(action=Action.RESPONSE, result=text, response=text)
