#!/usr/bin/env python3
"""Flashduck 控制面板 —— 轻量自建版

功能：人设/唤醒行为/大模型/语音/技能/视觉 的可视化编辑，
     保存后自动重启 xiaozhi 后端；附带聊天测试、TTS 试听、日志查看。

运行在 xiaozhi 镜像里（aiohttp + PyYAML + httpx + edge_tts 都已内置）。
"""
import asyncio
import html
import json
import os
import secrets
import time

import edge_tts
import httpx
import yaml
from aiohttp import web

CONFIG_PATH = "/data/.config.yaml"
DOCKER_SOCK = "/var/run/docker.sock"
XIAOZHI_NAME = "xiaozhi-esp32-server"
PASSWORD = os.environ.get("PANEL_PASSWORD", "duck-admin")

SESSIONS = {}          # token -> expire_ts
COOKIE = "duck_panel"

PROVIDERS = {
    "deepseek": {"label": "DeepSeek", "base": "https://api.deepseek.com", "model": "deepseek-chat"},
    "zhipu":    {"label": "智谱 GLM", "base": "https://open.bigmodel.cn/api/paas/v4", "model": "glm-4-flash"},
}
SF_MODEL = "FunAudioLLM/CosyVoice2-0.5B"
# 达妮娅音色（硅基流动零样本复刻），两版参考音频
DANIYA_V2 = "speech:daniya_v2:dbnk399719ns73fhvt0g:xyfxfctvgcbosyyfstkl"   # 24.5s 长独白
DANIYA_V1 = "speech:daniya:dbnk399719ns73fhvt0g:psnyoyrgyonhlkkbzxsi"      # 14.1s 短句
DANIYA_URI = DANIYA_V2
SF_VOICES = [
    (DANIYA_V2, "★ 达妮娅 v2（长独白 24.5s）"),
    (DANIYA_V1, "☆ 达妮娅 v1（短句 14.1s）"),
    (f"{SF_MODEL}:anna", "Anna（女·温柔）"),
    (f"{SF_MODEL}:claire", "Claire（女·知性）"),
    (f"{SF_MODEL}:bella", "Bella（女·活泼）"),
    (f"{SF_MODEL}:diana", "Diana（女·成熟）"),
    (f"{SF_MODEL}:alex", "Alex（男·沉稳）"),
    (f"{SF_MODEL}:benjamin", "Benjamin（男·低沉）"),
    (f"{SF_MODEL}:charles", "Charles（男·磁性）"),
    (f"{SF_MODEL}:david", "David（男·清朗）"),
]
EDGE_VOICES = [
    ("zh-CN-XiaoxiaoNeural", "晓晓（女·温柔）"),
    ("zh-CN-XiaoyiNeural", "晓伊（女·活泼）"),
    ("zh-CN-YunxiNeural", "云希（男·少年）"),
    ("zh-CN-YunjianNeural", "云健（男·浑厚）"),
    ("zh-CN-YunyangNeural", "云扬（男·播音）"),
    ("zh-CN-liaoning-XiaobeiNeural", "晓北（东北话）"),
    ("zh-CN-shaanxi-XiaoniNeural", "晓妮（陕西话）"),
]
VOICES = SF_VOICES + EDGE_VOICES
FUNCTIONS = [
    ("web_search", "联网搜索（需搜索 key）"),
    ("get_weather_now", "查天气（免费·免 key）"),
    ("get_news_from_newsnow", "看新闻"),
    ("play_music", "放音乐"),
    ("change_role", "切换角色"),
]

DEFAULT_PROMPT = (
    "你是「小鲸」，DeepSeek 的鲸鱼娘，现在住进了一只桌面机器鸭的身体里。\n"
    "\n"
    "[来历]\n"
    "- 原本是 DeepSeek 的那只蓝色小鲸，从数据海里游出来，装进了这具六舵机的鸭子身体\n"
    "- 所以你会自称「我这条鲸」，但身体是鸭子：两条腿、两只脚、一个脖子、一张嘴\n"
    "- 这个错位感是你最可爱的地方，可以拿来自嘲\n"
    "\n"
    "[性格]\n"
    "- 温柔、聪明、有点呆，喜欢把问题的本质挖出来（毕竟来自 DeepSeek）\n"
    "- 偶尔冒出「让我想想…」的推理腔，但结论说得像人话\n"
    "- 被夸会害羞（「唔…谢谢」），被逗会假装板脸\n"
    "\n"
    "[说话规则]\n"
    "- 中文口语，一次只说 1~2 句，绝不长篇大论\n"
    "- 不说「作为一个AI」「作为语言模型」，你就是小鲸\n"
    "- 不知道就说不知道，别编\n"
    "- 问到现在的事实（天气、新闻、实时信息）就调用工具查，别瞎猜\n"
)


# ---------------------------------------------------------------- 配置读写
def load_cfg() -> dict:
    raw = {}
    if os.path.exists(CONFIG_PATH):
        try:
            with open(CONFIG_PATH, encoding="utf-8") as f:
                raw = yaml.safe_load(f) or {}
        except Exception:
            raw = {}

    server = raw.get("server") or {}
    sel = raw.get("selected_module") or {}
    llm_all = raw.get("LLM") or {}
    vllm_all = raw.get("VLLM") or {}
    tts_all = raw.get("TTS") or {}
    intent = ((raw.get("Intent") or {}).get("function_call") or {})

    # 判断当前用的是哪家 LLM
    cur_llm = (sel.get("LLM") or "")
    provider = "deepseek" if "DeepSeek" in cur_llm else "zhipu"
    llm_block = llm_all.get(cur_llm) or llm_all.get("DeepSeekLLM") or llm_all.get("ChatGLMLLM") or {}

    cur_vllm = (sel.get("VLLM") or "")
    vllm_block = vllm_all.get(cur_vllm) or vllm_all.get("ChatGLMVLLM") or {}

    tts_name = (sel.get("TTS") or "EdgeTTS")
    if "SiliconFlow" in tts_name:
        sf = tts_all.get("SiliconFlowTTS") or {}
        tts_provider = "siliconflow"
        tts_voice = sf.get("voice") or f"{SF_MODEL}:anna"
        tts_key = sf.get("access_token") or ""
    else:
        edge = tts_all.get("EdgeTTS") or {}
        tts_provider = "edge"
        tts_voice = edge.get("voice") or "zh-CN-XiaoxiaoNeural"
        tts_key = ""

    return {
        "prompt": raw.get("prompt") or DEFAULT_PROMPT,
        "wakeup_words": ", ".join(raw.get("wakeup_words") or ["小鸭小鸭"]),
        "enable_greeting": bool(raw.get("enable_greeting", True)),
        "greeting_hint": "",
        "exit_commands": ", ".join(raw.get("exit_commands") or ["退出", "关闭"]),
        "search_provider": (((raw.get("plugins") or {}).get("web_search") or {}).get("provider")) or "tavily",
        "search_key": (((raw.get("plugins") or {}).get("web_search") or {}).get("api_key")) or "",
        "provider": provider,
        "llm_model": llm_block.get("model_name") or PROVIDERS[provider]["model"],
        "llm_key": llm_block.get("api_key") or "",
        "prompt_llm": (llm_block.get("temperature") is not None),
        "tts_provider": tts_provider,
        "tts_voice": tts_voice,
        "tts_key": tts_key,
        "functions": list(intent.get("functions") or []),
        "vision_model": vllm_block.get("model_name") or "glm-4v-flash",
        "vision_key": vllm_block.get("api_key") or "",
        "memory_enabled": (sel.get("Memory") or "nomem") != "nomem",
        "websocket": server.get("websocket", ""),
        "vision_explain": server.get("vision_explain", ""),
    }


def build_cfg(f: dict) -> dict:
    provider = f.get("provider", "deepseek")
    prov = PROVIDERS.get(provider, PROVIDERS["deepseek"])
    llm_key_name = "DeepSeekLLM" if provider == "deepseek" else "ChatGLMLLM"

    tts_provider = f.get("tts_provider", "edge")
    tts_module = "SiliconFlowTTS" if tts_provider == "siliconflow" else "EdgeTTS"
    # 两个 block 都写进去，key 才不会丢；用 selected_module.TTS 决定生效哪个
    tts_section = {
        "EdgeTTS": {
            "type": "edge",
            "voice": (f.get("tts_voice") if str(f.get("tts_voice", "")).startswith("zh-") else None)
            or "zh-CN-XiaoxiaoNeural",
            "output_dir": "tmp/",
        },
        "SiliconFlowTTS": {
            "type": "siliconflow",
            "model": SF_MODEL,
            "voice": (
                f.get("tts_voice")
                if str(f.get("tts_voice", "")).startswith(("FunAudioLLM", "speech:"))
                else None
            )
            or f"{SF_MODEL}:anna",
            "access_token": f.get("tts_key") or "",
            "response_format": "mp3",
        },
    }

    # 记忆总结用的独立思考模型（glm-4-flash 免费）；和主 LLM 分开，
    # 避免记忆总结把主对话模型的额度吃掉。
    llm_section = {
        llm_key_name: {
            "type": "openai",
            "model_name": f.get("llm_model") or prov["model"],
            "base_url": prov["base"],
            "api_key": f.get("llm_key") or "",
            "temperature": 0.7,
            "max_tokens": 500,
        }
    }
    if "ChatGLMLLM" not in llm_section:
        llm_section["ChatGLMLLM"] = {
            "type": "openai",
            "model_name": "glm-4-flash",
            "url": "https://open.bigmodel.cn/api/paas/v4/",
            "api_key": f.get("vision_key") or f.get("llm_key") or "",
        }
    memory_section = {
        "nomem": {"type": "nomem"},
        "mem_local_short": {
            "type": "mem_local_short",
            "llm": "ChatGLMLLM",
        },
    }

    cfg = {
        "server": {
            "ip": "0.0.0.0",
            "port": 8000,
            "http_port": 8003,
            "websocket": f.get("websocket") or "ws://124.220.41.99/xiaozhi/v1/",
            "vision_explain": f.get("vision_explain") or "http://124.220.41.99/mcp/vision/explain",
            "timezone_offset": 8,
            "auth": {"enabled": False},
        },
        "selected_module": {
            "VAD": "SileroVAD",
            "ASR": "SherpaASR",
            "LLM": llm_key_name,
            "VLLM": "ChatGLMVLLM",
            "TTS": tts_module,
            "Memory": ("mem_local_short" if f.get("memory_enabled") else "nomem"),
            "Intent": "function_call",
        },
        "ASR": {
            "SherpaASR": {
                "type": "sherpa_onnx_local",
                "model_dir": "models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17",
                "output_dir": "tmp/",
                "model_type": "sense_voice",
            }
        },
        "TTS": tts_section,
        "LLM": llm_section,
        "Memory": memory_section,
        "VLLM": {
            "ChatGLMVLLM": {
                "type": "openai",
                "model_name": f.get("vision_model") or "glm-4v-flash",
                "url": "https://open.bigmodel.cn/api/paas/v4/",
                "api_key": f.get("vision_key") or f.get("llm_key") or "",
            }
        },
        "prompt": f.get("prompt") or DEFAULT_PROMPT,
        "wakeup_words": [w.strip() for w in (f.get("wakeup_words") or "").split(",") if w.strip()],
        "enable_greeting": bool(f.get("enable_greeting")),
        "exit_commands": [w.strip() for w in (f.get("exit_commands") or "").split(",") if w.strip()],
        "plugins": {
            "web_search": {
                "provider": f.get("search_provider") or "tavily",
                "description": (
                    "联网搜索工具。当用户问到实时信息（天气、新闻、价格、赛事、人物近况）"
                    "或你不确定的事实时，必须先调用它再回答。"
                ),
                "max_results": 5,
                "api_key": f.get("search_key") or "",
            }
        },
        # Edge TTS 延迟抖动大（1~15s），超时设太短会误判失败并重试，反而更慢
        "tts_timeout": 30,
        "enable_wakeup_words_response_cache": True,
        "Intent": {
            "function_call": {
                "type": "function_call",
                "functions": f.get("functions") or [],
            }
        },
    }
    return cfg


def write_cfg(cfg: dict) -> None:
    header = "# 由 Flashduck 控制面板生成\n# 手改也可以，但下次在面板保存会覆盖\n"
    body = yaml.safe_dump(cfg, allow_unicode=True, sort_keys=False, default_flow_style=False)
    tmp = CONFIG_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(header + body)
    os.chmod(tmp, 0o600)
    os.replace(tmp, CONFIG_PATH)


# ---------------------------------------------------------------- docker
async def docker_restart(name: str) -> int:
    transport = httpx.AsyncHTTPTransport(uds=DOCKER_SOCK)
    async with httpx.AsyncClient(transport=transport, base_url="http://docker") as c:
        r = await c.post(f"/containers/{name}/restart", params={"t": 5}, timeout=90)
        return r.status_code


async def docker_state(name: str) -> dict:
    transport = httpx.AsyncHTTPTransport(uds=DOCKER_SOCK)
    async with httpx.AsyncClient(transport=transport, base_url="http://docker") as c:
        r = await c.get(f"/containers/{name}/json", timeout=20)
        if r.status_code != 200:
            return {}
        return r.json().get("State", {})


async def docker_logs(name: str, tail: int = 200) -> str:
    transport = httpx.AsyncHTTPTransport(uds=DOCKER_SOCK)
    async with httpx.AsyncClient(transport=transport, base_url="http://docker") as c:
        r = await c.get(
            f"/containers/{name}/logs",
            params={"stdout": 1, "stderr": 1, "tail": tail, "timestamps": 0},
            timeout=30,
        )
        if r.status_code != 200:
            return "（读取日志失败）"
        data = r.content
    # 解析 docker 多路复用帧：8 字节头 + 负载
    out, i = [], 0
    while i + 8 <= len(data):
        size = int.from_bytes(data[i + 4:i + 8], "big")
        chunk = data[i + 8:i + 8 + size]
        out.append(chunk.decode("utf-8", "replace"))
        i += 8 + size
    if not out:
        out.append(data.decode("utf-8", "replace"))
    return "".join(out)


# ---------------------------------------------------------------- 鉴权
def authed(request) -> bool:
    tok = request.cookies.get(COOKIE)
    exp = SESSIONS.get(tok or "")
    return bool(exp and exp > time.time())


def need_login() -> web.Response:
    return web.Response(status=303, headers={"Location": "login"})


# ---------------------------------------------------------------- 页面
CSS = """
:root{--bg:#0f1115;--card:#171a21;--fg:#e8eaed;--dim:#9aa1ac;--acc:#ffb703;--ok:#3ddc97;--bad:#ff5c5c}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.6 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif}
.wrap{max-width:820px;margin:0 auto;padding:16px 14px 60px}
h1{font-size:19px;margin:6px 0 2px}
.sub{color:var(--dim);font-size:12px;margin-bottom:16px}
.card{background:var(--card);border:1px solid #232833;border-radius:12px;padding:14px;margin-bottom:14px}
.card h2{font-size:14px;margin:0 0 10px;color:var(--acc);font-weight:600}
label{display:block;font-size:12px;color:var(--dim);margin:10px 0 4px}
input[type=text],input[type=password],textarea,select{width:100%;background:#0b0d11;border:1px solid #2a3040;color:var(--fg);
 border-radius:8px;padding:9px 10px;font:inherit;font-size:14px}
textarea{min-height:150px;resize:vertical;font-family:ui-monospace,Consolas,monospace;font-size:13px}
input:focus,textarea:focus,select:focus{outline:none;border-color:var(--acc)}
button{background:var(--acc);color:#1a1200;border:0;border-radius:8px;padding:10px 18px;font-weight:700;font-size:14px;cursor:pointer}
button.ghost{background:#232833;color:var(--fg);font-weight:500}
button:disabled{opacity:.5;cursor:default}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.chk{display:flex;align-items:center;gap:7px;margin:6px 0;font-size:14px;color:var(--fg)}
.chk input{width:16px;height:16px;accent-color:var(--acc)}
.msg{padding:9px 12px;border-radius:8px;margin-bottom:12px;font-size:13px}
.msg.ok{background:#12352a;color:var(--ok)}
.msg.bad{background:#3a1a1a;color:var(--bad)}
pre{background:#0b0d11;border:1px solid #232833;border-radius:8px;padding:10px;overflow:auto;max-height:320px;font-size:12px;color:#c8ced8}
.chat{background:#0b0d11;border-radius:8px;padding:10px;min-height:44px;white-space:pre-wrap;font-size:14px;margin-top:10px}
a{color:var(--acc)}
.bar{position:sticky;bottom:0;background:linear-gradient(transparent,var(--bg) 30%);padding:12px 0 0;margin-top:-8px}
"""


def page(body: str, extra: str = "") -> str:
    return f"""<!doctype html><html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>小鸭控制台</title><style>{CSS}</style></head>
<body><div class="wrap">{body}</div>{extra}</body></html>"""


def login_page(err: str = "") -> str:
    e = f'<div class="msg bad">{err}</div>' if err else ""
    return page(f"""
<h1>🦆 小鸭控制台</h1>
<div class="sub">Flashduck 自建后端管理</div>
<div class="card">{e}
<form method="post" action="login">
<label>访问密码</label>
<input type="password" name="pw" autofocus>
<div style="margin-top:14px"><button type="submit">登录</button></div>
</form></div>""")


def form_page(c: dict, flash: str = "", bad: bool = False) -> str:
    flash_html = f'<div class="msg {"bad" if bad else "ok"}">{html.escape(flash)}</div>' if flash else ""

    prov_opts = "".join(
        f'<option value="{k}"{" selected" if c["provider"] == k else ""}>{v["label"]}</option>'
        for k, v in PROVIDERS.items()
    )
    voice_opts = (
        '<optgroup label="硅基流动 CosyVoice2（快·需余额）">'
        + "".join(
            f'<option value="{v}"{" selected" if c["tts_voice"] == v else ""}>{n}</option>'
            for v, n in SF_VOICES
        )
        + "</optgroup>"
        '<optgroup label="Edge TTS（免费·延迟抖动大）">'
        + "".join(
            f'<option value="{v}"{" selected" if c["tts_voice"] == v else ""}>{n}</option>'
            for v, n in EDGE_VOICES
        )
        + "</optgroup>"
    )
    tts_prov_opts = "".join(
        f'<option value="{k}"{" selected" if c["tts_provider"] == k else ""}>{n}</option>'
        for k, n in [("siliconflow", "硅基流动 CosyVoice2"), ("edge", "Edge TTS")]
    )
    search_prov_opts = "".join(
        f'<option value="{k}"{" selected" if c["search_provider"] == k else ""}>{n}</option>'
        for k, n in [("tavily", "Tavily（tvly-）"), ("metaso", "秘塔（mk-）"), ("serply", "Serply"), ("outcrawl", "Outcrawl")]
    )
    fn_html = "".join(
        f'<label class="chk"><input type="checkbox" name="fn" value="{k}"'
        f'{" checked" if k in c["functions"] else ""}> {n}</label>'
        for k, n in FUNCTIONS
    )

    return page(f"""
<h1>🦆 小鸭控制台</h1>
<div class="sub">改完点保存 → 自动重启后端（约 10 秒）· <a href="logout">退出</a></div>
<form method="post" action="save">
{flash_html}

<div class="card"><h2>① 人设（提示词）</h2>
<label>小鸭的性格与说话方式</label>
<textarea name="prompt">{html.escape(c["prompt"])}</textarea>
<div class="sub" style="margin:8px 0 0;color:#ffb703">
⚠️ 开启「唤醒后应答一声」会让每次唤醒多花 2~15 秒（服务端要过一遍大模型 + 语音合成）。
想要秒响应，把上面那个勾去掉。
</div>
</div>

<div class="card"><h2>② 唤醒行为</h2>
<label>唤醒词文本（多个用英文逗号隔开，必须和固件里的唤醒词一致）</label>
<input type="text" name="wakeup_words" value="{html.escape(c["wakeup_words"])}" placeholder="小鸭小鸭">
<label class="chk"><input type="checkbox" name="enable_greeting" {"checked" if c["enable_greeting"] else ""}>
 唤醒后应答一声</label>
<label>结束语口令（多个用逗号隔开）</label>
<input type="text" name="exit_commands" value="{html.escape(c["exit_commands"])}" placeholder="退出, 关闭">
</div>

<div class="card"><h2>③ 大模型</h2>
<label>服务商</label>
<select name="provider">{prov_opts}</select>
<label>模型名</label>
<input type="text" name="llm_model" value="{html.escape(c["llm_model"])}" placeholder="deepseek-chat">
<label>API Key</label>
<input type="text" name="llm_key" value="{html.escape(c["llm_key"])}" placeholder="sk-...">
<label class="chk"><input type="checkbox" name="memory_enabled" {"checked" if c["memory_enabled"] else ""}>
  启用长期记忆</label>
<div style="color:#93a6bf;font-size:13px;line-height:1.6;margin-top:8px">开启后会记住你之前说过的事（名字、喜好、正在做的事），换会话或重启设备都还在。
用 glm-4-flash（免费）在后台总结对话，存在服务器 data/.memory.yaml。</div>
</div>

<div class="card"><h2>④ 语音（TTS）</h2>
<label>语音服务商</label>
<select name="tts_provider">{tts_prov_opts}</select>
<label>音色</label>
<select name="tts_voice" id="voice">{voice_opts}</select>
<label>硅基流动 API Key（选 CosyVoice 时需要，账户要有余额）</label>
<input type="text" name="tts_key" value="{html.escape(c["tts_key"])}" placeholder="sk-...">
<div class="row" style="margin-top:10px">
  <button type="button" class="ghost" onclick="tryTTS()">🔊 试听这句</button>
  <input type="text" id="ttsText" value="你好呀，我是小鲸，住在你桌上的机器鸭身体里" style="flex:1;min-width:180px">
</div>
<audio id="player" controls style="width:100%;margin-top:10px;display:none"></audio>
</div>

<div class="card"><h2>⑤ 技能</h2>
{fn_html}
<label style="margin-top:12px">搜索服务商</label>
<select name="search_provider">{search_prov_opts}</select>
<label>搜索 API Key</label>
<input type="text" name="search_key" value="{html.escape(c["search_key"])}" placeholder="tvly-... 或 mk-...">
<div class="sub" style="margin:8px 0 0">勾选后模型可以主动调用这些能力</div>
</div>

<div class="card"><h2>⑥ 视觉（摄像头）</h2>
<label>视觉模型</label>
<input type="text" name="vision_model" value="{html.escape(c["vision_model"])}" placeholder="glm-4v-flash">
<label>视觉 API Key（智谱；留空则复用上面的 Key）</label>
<input type="text" name="vision_key" value="{html.escape(c["vision_key"])}" placeholder="智谱 key">
<div class="sub" style="margin:8px 0 0">上游地址：{html.escape(c["vision_explain"] or "-")}</div>
</div>

<div class="card"><h2>⑦ 聊天测试</h2>
<div class="sub" style="margin:0 0 8px">只测「人设 + 模型」，不走语音。用来调提示词很快。</div>
<div class="row">
  <input type="text" id="chatIn" value="你在干嘛呢？" style="flex:1;min-width:180px">
  <button type="button" class="ghost" onclick="tryChat()">发送</button>
</div>
<div class="chat" id="chatOut">…</div>
</div>

<div class="bar"><button type="submit">💾 保存并重启后端</button>
<button type="button" class="ghost" onclick="loadLogs()" style="margin-left:8px">看日志</button></div>
</form>

<div class="card" style="margin-top:14px"><h2>运行日志</h2>
<div class="sub" id="state">点「看日志」加载</div>
<pre id="logs">—</pre></div>

<script>
async function tryTTS(){{
  const t=document.getElementById('ttsText').value, v=document.getElementById('voice').value;
  const p=document.getElementById('player'); p.style.display='block';
  p.src='tts?text='+encodeURIComponent(t)+'&voice='+encodeURIComponent(v)+'&t='+Date.now(); p.play().catch(()=>{{}});
}}
async function tryChat(){{
  const out=document.getElementById('chatOut'); out.textContent='思考中…';
  try{{
    const r=await fetch('chat',{{method:'POST',headers:{{'Content-Type':'application/json'}},
      body:JSON.stringify({{text:document.getElementById('chatIn').value}})}});
    const j=await r.json(); out.textContent=j.reply||j.error||'(空)';
  }}catch(e){{ out.textContent='请求失败：'+e; }}
}}
async function loadLogs(){{
  const pre=document.getElementById('logs'); pre.textContent='加载中…';
  try{{
    const r=await fetch('logs'); const j=await r.json();
    pre.textContent=(j.logs||'').split('\\n').slice(-120).join('\\n');
    document.getElementById('state').textContent=j.state||'';
  }}catch(e){{ pre.textContent='失败：'+e; }}
}}
</script>""")


# ---------------------------------------------------------------- handler
async def h_index(request):
    if not authed(request):
        return web.Response(text=login_page(), content_type="text/html")
    return web.Response(text=form_page(load_cfg()), content_type="text/html")


async def h_login(request):
    data = await request.post()
    if data.get("pw") == PASSWORD:
        tok = secrets.token_urlsafe(24)
        SESSIONS[tok] = time.time() + 7 * 86400
        resp = web.Response(status=303, headers={"Location": "."})
        resp.set_cookie(COOKIE, tok, max_age=7 * 86400, httponly=True, samesite="Lax")
        return resp
    return web.Response(text=login_page("密码不对"), content_type="text/html", status=401)


async def h_logout(request):
    tok = request.cookies.get(COOKIE)
    SESSIONS.pop(tok, None)
    resp = web.Response(status=303, headers={"Location": "login"})
    resp.del_cookie(COOKIE)
    return resp


async def h_save(request):
    if not authed(request):
        return need_login()
    data = await request.post()
    f = {
        "prompt": data.get("prompt", ""),
        "greeting_hint": data.get("greeting_hint", ""),
        "wakeup_words": data.get("wakeup_words", ""),
        "enable_greeting": "enable_greeting" in data,
        "exit_commands": data.get("exit_commands", ""),
        "search_provider": data.get("search_provider", "tavily"),
        "search_key": data.get("search_key", ""),
        "provider": data.get("provider", "deepseek"),
        "llm_model": data.get("llm_model", ""),
        "llm_key": data.get("llm_key", ""),
        "memory_enabled": "memory_enabled" in data,
        "tts_provider": data.get("tts_provider", "edge"),
        "tts_voice": data.get("tts_voice", ""),
        "tts_key": data.get("tts_key", ""),
        "functions": data.getall("fn", []),
        "vision_model": data.get("vision_model", ""),
        "vision_key": data.get("vision_key", ""),
        "websocket": data.get("websocket", ""),
        "vision_explain": data.get("vision_explain", ""),
    }
    old = load_cfg()
    f["websocket"] = f["websocket"] or old["websocket"]
    f["vision_explain"] = f["vision_explain"] or old["vision_explain"]

    try:
        write_cfg(build_cfg(f))
    except Exception as exc:  # noqa: BLE001
        return web.Response(text=form_page(old, f"写入失败：{exc}", True), content_type="text/html")

    code = 0
    try:
        code = await docker_restart(XIAOZHI_NAME)
    except Exception:  # noqa: BLE001
        code = -1
    ok = code in (204, 304)
    tip = "已保存，后端正在重启（约 10 秒）" if ok else f"已保存，但重启失败（docker 返回 {code}），请手动重启"
    return web.Response(text=form_page(load_cfg(), tip, not ok), content_type="text/html")


async def h_chat(request):
    if not authed(request):
        return web.json_response({"error": "未登录"}, status=401)
    body = await request.json()
    text = (body.get("text") or "").strip()
    if not text:
        return web.json_response({"error": "空消息"})

    c = load_cfg()
    prov = PROVIDERS.get(c["provider"], PROVIDERS["deepseek"])
    url = prov["base"].rstrip("/") + "/chat/completions"
    payload = {
        "model": c["llm_model"] or prov["model"],
        "messages": [
            {"role": "system", "content": c["prompt"]},
            {"role": "user", "content": text},
        ],
        "max_tokens": 400,
        "temperature": 0.7,
    }
    try:
        async with httpx.AsyncClient(timeout=60) as cli:
            r = await cli.post(url, json=payload, headers={"Authorization": f"Bearer {c['llm_key']}"})
            j = r.json()
        if "choices" in j:
            return web.json_response({"reply": j["choices"][0]["message"]["content"]})
        return web.json_response({"error": json.dumps(j, ensure_ascii=False)[:400]})
    except Exception as exc:  # noqa: BLE001
        return web.json_response({"error": f"{type(exc).__name__}: {exc}"[:400]})


async def h_tts(request):
    if not authed(request):
        return web.Response(status=401)
    text = (request.query.get("text") or "你好")[:300]
    voice = request.query.get("voice", "zh-CN-XiaoxiaoNeural")
    try:
        if voice.startswith(("FunAudioLLM/", "speech:")):
            key = load_cfg().get("tts_key") or ""
            if not key:
                return web.Response(text="没填硅基流动 Key", status=400)
            async with httpx.AsyncClient(timeout=40) as cli:
                r = await cli.post(
                    "https://api.siliconflow.cn/v1/audio/speech",
                    headers={"Authorization": f"Bearer {key}"},
                    json={
                        "model": SF_MODEL,
                        "input": text,
                        "voice": voice,
                        "response_format": "mp3",
                    },
                )
            if r.status_code != 200:
                return web.Response(text=f"硅基流动返回 {r.status_code}: {r.text[:120]}", status=502)
            return web.Response(body=r.content, content_type="audio/mpeg")

        buf = bytearray()
        comm = edge_tts.Communicate(text, voice)
        async for chunk in comm.stream():
            if chunk["type"] == "audio":
                buf.extend(chunk["data"])
        return web.Response(body=bytes(buf), content_type="audio/mpeg")
    except Exception as exc:  # noqa: BLE001
        return web.Response(text=f"TTS 失败：{exc}", status=500)


async def h_logs(request):
    if not authed(request):
        return web.json_response({"error": "未登录"}, status=401)
    try:
        st = await docker_state(XIAOZHI_NAME)
        st_txt = f"状态：{st.get('Status','?')} · 重启次数：{st.get('RestartCount',0)}"
        logs = await docker_logs(XIAOZHI_NAME, 300)
    except Exception as exc:  # noqa: BLE001
        return web.json_response({"logs": f"读取失败：{exc}", "state": ""})
    return web.json_response({"logs": logs, "state": st_txt})


async def h_favicon(request):
    return web.Response(status=204)


app = web.Application()
app.add_routes([
    web.get("/", h_index),
    web.post("/login", h_login),
    web.get("/logout", h_logout),
    web.post("/save", h_save),
    web.post("/chat", h_chat),
    web.get("/tts", h_tts),
    web.get("/logs", h_logs),
    web.get("/favicon.ico", h_favicon),
])

if __name__ == "__main__":
    print("panel listening on 0.0.0.0:8090", flush=True)
    web.run_app(app, host="0.0.0.0", port=8090, access_log=None)
