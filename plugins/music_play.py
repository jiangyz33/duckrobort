"""音乐播放插件

原理：设备端的 NotifyPlayer 只支持 OGG/Opus 流，所以：
  1. 曲库存成 .ogg（放在 data/music/，由网关以 /music/ 静态托管）
  2. 插件按关键词挑一首，给设备发一条 notify 消息（{"type":"notify","audio_url":...}）
  3. 设备下载并播放，状态进入 Notifying，放完自动回 Idle

"停止播放"用的是播放一段极短静音的做法——NotifyPlayer 只认 audio_url，
收到新的 notify 会顶掉当前正在播的内容。
"""

import json
import os
import urllib.parse

from plugins_func.register import Action, ActionResponse, ToolType, register_function

MUSIC_DIR = "/opt/xiaozhi-esp32-server/data/music"
# 设备能访问到的公网前缀；换服务器时改这里
MUSIC_BASE = "http://124.220.41.99/music/"
SILENCE_NAME = "_silence.ogg"

MUSIC_PLAY_DESC = {
    "type": "function",
    "function": {
        "name": "self_music_play",
        "description": (
            "播放音乐。用户说「放首歌」「来点音乐」「我想听XX」「放点轻音乐」时调用。"
            "keyword 是歌曲名/歌手名/风格关键词，留空表示随便放一首。"
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "keyword": {
                    "type": "string",
                    "description": "歌曲或歌手关键词，可为空字符串",
                }
            },
            "required": [],
        },
    },
}

MUSIC_LIST_DESC = {
    "type": "function",
    "function": {
        "name": "self_music_list",
        "description": "列出音乐库里有哪些歌。用户问「你能放什么歌」「曲库有什么」时调用。",
        "parameters": {"type": "object", "properties": {}, "required": []},
    },
}

MUSIC_STOP_DESC = {
    "type": "function",
    "function": {
        "name": "self_music_stop",
        "description": "停止当前正在播放的音乐。用户说「别放了」「停」「关掉音乐」时调用。",
        "parameters": {"type": "object", "properties": {}, "required": []},
    },
}


def _tracks():
    if not os.path.isdir(MUSIC_DIR):
        return []
    out = []
    for name in os.listdir(MUSIC_DIR):
        if not name.lower().endswith(".ogg"):
            continue
        if name == SILENCE_NAME:
            continue
        path = os.path.join(MUSIC_DIR, name)
        if os.path.isfile(path) and os.path.getsize(path) > 1024:
            out.append(name)
    out.sort()
    return out


def _pretty(name):
    return os.path.splitext(name)[0]


def _pick(keyword, tracks):
    """关键词模糊匹配：先精确包含，再按字符命中率打分。"""
    kw = (keyword or "").strip().lower()
    if not kw:
        return tracks[0] if tracks else None
    scored = []
    for name in tracks:
        stem = _pretty(name).lower()
        if kw in stem:
            return name
        hit = sum(1 for ch in kw if ch in stem)
        if hit:
            scored.append((hit / max(len(kw), 1), name))
    if not scored:
        return None
    scored.sort(reverse=True)
    return scored[0][1]


async def _send_notify(conn, url, text):
    payload = {
        "type": "notify",
        "state": "start",
        "audio_url": url,
        "subtitles": [{"start_ms": 0, "text": text}] if text else [],
    }
    await conn.websocket.send(json.dumps(payload, ensure_ascii=False))
    # 关键：让服务端在本次回复播完后挂断连接。
    # 设备收到 notify 时如果还在对话中，会先排队；连接一断设备回到 Idle，
    # 排队的 notify 才会真正开始播放。否则要等会话超时(120s)才响。
    try:
        conn.close_after_chat = True
    except Exception:  # noqa: BLE001
        pass


@register_function("self_music_play", MUSIC_PLAY_DESC, ToolType.SYSTEM_CTL)
async def play_music(conn, keyword: str = ""):
    tracks = _tracks()
    if not tracks:
        return ActionResponse(
            action=Action.RESPONSE,
            response="音乐库里还没有歌，让我主人先放几首进去吧。",
        )

    name = _pick(keyword, tracks)
    if name is None:
        return ActionResponse(
            action=Action.RESPONSE,
            response=f"曲库里没有和「{keyword}」对得上的歌，换个说法试试？",
        )

    url = MUSIC_BASE + urllib.parse.quote(name)
    title = _pretty(name)
    try:
        await _send_notify(conn, url, title)
    except Exception as exc:  # noqa: BLE001
        return ActionResponse(
            action=Action.RESPONSE, response=f"播放请求发送失败：{exc}"
        )

    total = len(tracks)
    if keyword:
        return ActionResponse(
            action=Action.RESPONSE, response=f"好，放「{title}」给你听（曲库共 {total} 首）。"
        )
    return ActionResponse(
        action=Action.RESPONSE, response=f"随机挑了一首「{title}」，一起听吧。"
    )


@register_function("self_music_list", MUSIC_LIST_DESC, ToolType.SYSTEM_CTL)
async def list_music(conn):
    tracks = _tracks()
    if not tracks:
        return ActionResponse(action=Action.RESPONSE, response="曲库还是空的。")
    names = "、".join(_pretty(n) for n in tracks[:10])
    more = f"等 {len(tracks)} 首" if len(tracks) > 10 else f"共 {len(tracks)} 首"
    return ActionResponse(action=Action.RESPONSE, response=f"曲库里有：{names}，{more}。")


@register_function("self_music_stop", MUSIC_STOP_DESC, ToolType.SYSTEM_CTL)
async def stop_music(conn):
    silence = os.path.join(MUSIC_DIR, SILENCE_NAME)
    if not os.path.isfile(silence):
        return ActionResponse(action=Action.RESPONSE, response="（静音文件缺失，停不了）")
    url = MUSIC_BASE + SILENCE_NAME
    try:
        await _send_notify(conn, url, "")
    except Exception as exc:  # noqa: BLE001
        return ActionResponse(action=Action.RESPONSE, response=f"停止失败：{exc}")
    return ActionResponse(action=Action.RESPONSE, response="好，关掉了。")
