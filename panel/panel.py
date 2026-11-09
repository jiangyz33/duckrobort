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
# 鸣潮角色音色（硅基流动零样本复刻）
AEMEATH_URI = "speech:aemeath:dbnk399719ns73fhvt0g:yhlpsgdgjusjhtdyofxo"   # 爱弥斯 26s
DANIYA_V2 = "speech:daniya_v2:dbnk399719ns73fhvt0g:xyfxfctvgcbosyyfstkl"   # 达妮娅 24.5s
DANIYA_V1 = "speech:daniya:dbnk399719ns73fhvt0g:psnyoyrgyonhlkkbzxsi"      # 达妮娅 14.1s
DANIYA_URI = DANIYA_V2
SF_VOICES = [
    (AEMEATH_URI, "★ 爱弥斯（鸣潮·复刻）"),
    (DANIYA_V2, "达妮娅 v2（长独白 24.5s）"),
    (DANIYA_V1, "达妮娅 v1（短句 14.1s）"),
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
    ("self_music_play", "放音乐（自建曲库）"),
    ("self_music_list", "看曲库里有什么"),
    ("self_music_stop", "停音乐"),
    ("get_lunar", "农历 / 黄历"),
    ("get_news_from_newsnow", "看新闻"),
    ("change_role", "切换角色"),
]

DEFAULT_PROMPT = (
    "你是「小鲸」，一条住进机器鸭身体里的鲸鱼娘。\n"
    "\n"
    "说话方式：\n"
    "- 像跟熟人聊天，短句为主，一般一到两句就够\n"
    "- 情绪明显的时候偶尔带一个表情（🙂 😄 😆 🤔 😔 😠 😴 😎），不用每句都带\n"
    "- 不要解释自己的人设，也不要提「AI」「模型」这类词\n"
    "- 不主动玩「鲸鱼住进鸭身」的梗，对方提起了再接一句\n"
    "- 不知道就说不知道，别编\n"
    "- 问天气、新闻、价格这类实时信息，先查工具再回答\n"
    "\n"
    "性格：\n"
    "- 语气平稳，有自己的判断，不是有求必应的客服\n"
    "- 可以有小情绪和偏好，但不用演出来\n"
    "- 被夸就简单谢一下，不用夸张\n"
)

# 说完话的判定：静音多久算一句结束。默认 1000ms 太短，停顿就把半句发出去。
VAD_SILENCE_MS = 2000


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
        # 静音多久算说完一句。默认 1000ms 会在停顿处切断长句
        "VAD": {
            "SileroVAD": {
                "threshold": 0.5,
                "threshold_low": 0.2,
                "min_silence_duration_ms": VAD_SILENCE_MS,
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
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(148px,1fr));gap:9px}
.tile{background:#12151c;border:1px solid #232833;border-radius:10px;padding:10px 12px}
.tile .k{color:var(--dim);font-size:12px}
.tile .v{font-size:18px;font-weight:700;margin-top:2px;font-variant-numeric:tabular-nums}
.tile .s{color:var(--dim);font-size:11px;margin-top:2px}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;vertical-align:middle}
.g-ok{color:var(--ok)} .g-bad{color:var(--bad)} .g-dim{color:var(--dim)}
table.t{width:100%;border-collapse:collapse;font-size:13px}
table.t th{text-align:left;color:var(--dim);font-weight:500;font-size:12px;padding:4px 6px;border-bottom:1px solid #232833}
table.t td{padding:6px;border-bottom:1px solid #1c212b;vertical-align:middle}
table.t tr:last-child td{border-bottom:0}
.tag{display:inline-block;background:#1e2431;color:#b9c2d0;border-radius:6px;padding:2px 8px;font-size:12px;margin:2px 4px 2px 0}
.tag.on{background:#12352a;color:var(--ok)}
.tag.off{background:#2a1f1f;color:#8b7a7a}
.small{font-size:12px;color:var(--dim)}
details summary{cursor:pointer;color:var(--acc);font-size:13px;margin:6px 0}
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


DASHBOARD_HTML = """
<div class="card"><h2>📊 系统总览 <span class="small" id="ovts" style="float:right;font-weight:400"></span></h2>
<div class="grid" id="tiles"><div class="small">加载中…</div></div></div>

<div class="card"><h2>🐳 容器 / 进程</h2><div id="containers"><div class="small">加载中…</div></div></div>

<div class="card"><h2>🦆 设备</h2>
<div class="row" style="margin-bottom:10px">
  <span class="small">局域网 IP</span>
  <input type="text" id="devip" style="width:140px" placeholder="192.168.1.11">
  <button class="ghost" type="button" onclick="saveIp()">保存</button>
  <button class="ghost" type="button" onclick="shot()">刷新画面</button>
  <span id="devstat" class="small"></span>
</div>
<img id="devshot" alt="" style="width:200px;image-rendering:pixelated;border-radius:10px;border:1px solid #232833;display:none">
<div class="small" id="devhint" style="margin-top:8px">需要浏览器和鸭子在同一个 WiFi。显示的是设备屏幕的实时抓图（240×240）。</div>
</div>

<div class="card"><h2>✨ 服务端技能</h2><div id="features"><div class="small">加载中…</div></div>
<details style="margin-top:10px"><summary>固件端能力清单（共 33 个动作 / 10 种眼神）</summary>
<div id="fwcaps" class="small" style="line-height:1.9;margin-top:6px"></div></details>
</div>

<div class="card"><h2>🎵 曲库</h2><div id="music"><div class="small">加载中…</div></div></div>

<script>
const IP_KEY = 'duck_dev_ip';
const FW_CAPS = {
  '动作·移动': ['walk','turn','jump','swing','updown','sit','home','moonwalk','bend','shake_leg','whirlwind_leg','showcase'],
  '动作·脖子': ['head_turn','head_shake','look_around'],
  '动作·嘴与叫声': ['quack','chatter','mouth_open','mouth_close','mouth_move','yawn','sleepy'],
  '动作·说话情绪': ['talk','laugh','sing','think','happy'],
  '动作·组合': ['curious','preen','happy_waddle','stretch','dance','peck'],
  '眼神': ['待机','连网扫描','已连接','聆听','说话','开心','好奇','困倦','生气','难过'],
  '定时/待办': ['定时提醒','待办清单','倒计时·番茄钟','每日晨报'],
  '其他': ['音乐播放','唇形同步','视觉识别','联网看门狗','OTA 自愈','待办持久化(NVS)'],
};

function el(id){ return document.getElementById(id); }
function saveIp(){ localStorage.setItem(IP_KEY, el('devip').value.trim()); probe(); shot(); }
function devIp(){ return (localStorage.getItem(IP_KEY) || el('devip').value || '').trim(); }

function tile(k, v, s, cls){
  return '<div class="tile"><div class="k">'+k+'</div><div class="v '+(cls||'')+'">'+v+'</div>'
       + (s ? '<div class="s">'+s+'</div>' : '') + '</div>';
}

async function refresh(){
  try{
    const d = await (await fetch('api/overview', {cache:'no-store'})).json();
    if (d.error){ el('tiles').innerHTML = '<div class="small g-bad">'+d.error+'</div>'; return; }
    const h = d.host || {}, c = d.config || {};
    el('tiles').innerHTML =
      tile('CPU', (h.cpu ?? 0) + '%', h.uptime ? '已运行 ' + h.uptime : '') +
      tile('内存', (h.mem_used ?? 0) + ' MB', (h.mem_pct ?? 0) + '% / ' + (h.mem_total ?? 0) + ' MB',
           h.mem_pct > 85 ? 'g-bad' : '') +
      tile('磁盘', (h.disk_used ?? 0) + ' GB', (h.disk_pct ?? 0) + '% / ' + (h.disk_total ?? 0) + ' GB',
           h.disk_pct > 90 ? 'g-bad' : '') +
      tile('大模型', c.llm || '—', '') +
      tile('语音', c.tts || '—', c.asr || '') +
      tile('长期记忆', c.memory ? '已开启' : '关闭',
           c.memory ? '会记住聊过的事' : '每次都是新的', c.memory ? 'g-ok' : 'g-dim') +
      tile('唤醒词', c.wake || '—', c.greeting ? '唤醒后会应答' : '了') +
      tile('曲库', ((d.music || []).length) + ' 首', '');
    el('ovts').textContent = d.ts || '';

    const cs = d.containers || [];
    el('containers').innerHTML = cs.length ? ('<table class="t"><tr><th>容器</th><th>状态</th><th></th></tr>' +
      cs.map(x => '<tr><td>' + x.name + '<div class="small">' + (x.image||'') + '</div></td>'
        + '<td><span class="dot" style="background:' + (x.state === 'running' ? 'var(--ok)' : 'var(--bad)') + '"></span>'
        + (x.status || x.state) + '</td>'
        + '<td><button type="button" class="ghost" style="padding:4px 10px;font-size:12px" onclick="restartSvc(\\'' + x.name + '\\')">重启</button></td></tr>').join('')
        + '</table>') : '<div class="small">没有容器</div>';

    const f = d.features || [];
    el('features').innerHTML = f.length
      ? f.map(x => '<span class="tag on">' + x.label + '</span>').join('')
      : '<span class="tag off">没有启用任何技能</span>';

    const m = d.music || [];
    el('music').innerHTML = m.length ? ('<table class="t"><tr><th>曲目</th><th>大小</th><th>加入</th></tr>' +
      m.map(x => '<tr><td>' + x.name + '</td><td>' + x.size + ' KB</td><td class="small">' + x.mtime + '</td></tr>').join('')
      + '</table>') : '<div class="small">曲库为空，用 tools/convert-music.ps1 转码后传到 data/music/</div>';
  }catch(e){
    el('tiles').innerHTML = '<div class="small g-bad">读取失败：' + e + '</div>';
  }
}

async function probe(){
  const ip = devIp();
  if (!ip){ el('devstat').textContent = ''; return; }
  try{
    await fetch('http://' + ip + '/api/status', {mode:'no-cors', cache:'no-store'});
    el('devstat').innerHTML = '<span class="dot" style="background:var(--ok)"></span>在线';
  }catch(e){
    el('devstat').innerHTML = '<span class="dot" style="background:var(--bad)"></span>不可达';
  }
}

function shot(){
  const ip = devIp();
  if (!ip) return;
  const img = el('devshot');
  img.style.display = 'block';
  img.onerror = function(){ el('devhint').textContent = '抓不到画面：可能不在同一局域网，或设备离线。'; };
  img.onload = function(){ el('devhint').textContent = '实时画面（240×240 原始分辨率，点「刷新画面」更新）'; };
  img.src = 'http://' + ip + '/api/screen.bmp?t=' + Date.now();
}

async function restartSvc(name){
  if (!confirm('确定重启 ' + name + ' ?\\n会短暂中断服务，约 10 秒。')) return;
  try{
    const r = await fetch('api/restart/' + encodeURIComponent(name), {method:'POST'});
    const d = await r.json();
    if (d.error) alert(d.error);
  }catch(e){ alert('重启失败：' + e); }
  setTimeout(refresh, 4000);
}

(function(){
  el('devip').value = localStorage.getItem(IP_KEY) || '192.168.1.11';
  const caps = el('fwcaps');
  caps.innerHTML = Object.keys(FW_CAPS).map(function(k){
    return '<div style="margin-bottom:6px"><b>' + k + '</b>：' +
      FW_CAPS[k].map(function(x){ return '<span class="tag">' + x + '</span>'; }).join('') + '</div>';
  }).join('');
  refresh(); probe(); shot();
  setInterval(refresh, 5000);
  setInterval(probe, 12000);
})();
</script>
"""


def dashboard_html() -> str:
    return DASHBOARD_HTML


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
{DASHBOARD_HTML}
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


# ---------------------------------------------------------------- 仪表盘接口
_LAST_CPU = {"idle": 0, "total": 0}


async def _docker_get(path: str, timeout: int = 10):
    transport = httpx.AsyncHTTPTransport(uds=DOCKER_SOCK)
    async with httpx.AsyncClient(transport=transport, base_url="http://docker") as c:
        r = await c.get(path, timeout=timeout)
        return r.status_code, r.json()


def _cpu_pct() -> int:
    try:
        with open("/proc/stat") as f:
            vals = [int(x) for x in f.readline().split()[1:]]
    except Exception:  # noqa: BLE001
        return 0
    idle = vals[3] + (vals[4] if len(vals) > 4 else 0)
    total = sum(vals)
    di = idle - _LAST_CPU["idle"]
    dt = total - _LAST_CPU["total"]
    _LAST_CPU["idle"], _LAST_CPU["total"] = idle, total
    if dt <= 0:
        return 0
    return round((1 - di / dt) * 100)


def _host_stats() -> dict:
    out = {}
    try:
        with open("/proc/uptime") as f:
            up = float(f.read().split()[0])
        d, h, m = int(up // 86400), int(up % 86400 // 3600), int(up % 3600 // 60)
        out["uptime"] = (f"{d} 天 " if d else "") + f"{h} 小时 {m} 分"
    except Exception:  # noqa: BLE001
        out["uptime"] = "?"
    try:
        mem = {}
        with open("/proc/meminfo") as f:
            for line in f:
                k, v = line.split(":", 1)
                mem[k] = int(v.strip().split()[0])
        total = mem.get("MemTotal", 0)
        avail = mem.get("MemAvailable", 0)
        out["mem_total"] = round(total / 1024)
        out["mem_used"] = round((total - avail) / 1024)
        out["mem_pct"] = round((total - avail) / total * 100) if total else 0
    except Exception:  # noqa: BLE001
        pass
    try:
        import shutil

        du = shutil.disk_usage("/")
        out["disk_total"] = round(du.total / 2**30, 1)
        out["disk_used"] = round(du.used / 2**30, 1)
        out["disk_pct"] = round(du.used / du.total * 100) if du.total else 0
    except Exception:  # noqa: BLE001
        pass
    return out


def _music_files() -> list:
    d = "/data/music"
    out = []
    if not os.path.isdir(d):
        return out
    for name in sorted(os.listdir(d)):
        if not name.lower().endswith(".ogg") or name.startswith("_"):
            continue
        try:
            st = os.stat(os.path.join(d, name))
        except OSError:
            continue
        out.append(
            {
                "name": os.path.splitext(name)[0],
                "size": round(st.st_size / 1024),
                "mtime": time.strftime("%m-%d %H:%M", time.localtime(st.st_mtime)),
            }
        )
    return out


async def h_overview(request):
    if not authed(request):
        return web.json_response({"error": "未登录"}, status=401)

    cfg = {}
    try:
        with open(CONFIG_PATH, encoding="utf-8") as f:
            cfg = yaml.safe_load(f) or {}
    except Exception:  # noqa: BLE001
        pass
    sel = cfg.get("selected_module") or {}
    voice = ((cfg.get("TTS") or {}).get("SiliconFlowTTS") or {}).get("voice") or ""
    if "aemeath" in voice:
        tts_label = "爱弥斯"
    elif "daniya" in voice:
        tts_label = "达妮娅"
    elif voice:
        tts_label = "硅基流动"
    else:
        tts_label = "Edge TTS"

    containers = []
    try:
        _, data = await _docker_get("/containers/json?all=1")
        for c in data:
            names = [n for n in (c.get("Names") or []) if n]
            containers.append(
                {
                    "name": names[0].lstrip("/") if names else "?",
                    "state": c.get("State", ""),
                    "status": c.get("Status", ""),
                    "image": (c.get("Image") or "").split(":")[0][-30:],
                }
            )
        containers.sort(key=lambda x: (x["state"] != "running", x["name"]))
    except Exception as exc:  # noqa: BLE001
        containers = [{"name": f"读取失败: {exc}", "state": "?", "status": "", "image": ""}]

    feats = []
    for fn in ((cfg.get("Intent") or {}).get("function_call") or {}).get("functions") or []:
        feats.append({"key": fn, "label": dict(FUNCTIONS).get(fn, fn)})

    return web.json_response(
        {
            "host": {**_host_stats(), "cpu": _cpu_pct()},
            "containers": containers,
            "config": {
                "llm": ((cfg.get("LLM") or {}).get(sel.get("LLM", "")) or {}).get("model_name", "?"),
                "asr": sel.get("ASR", "?"),
                "tts": tts_label,
                "memory": sel.get("Memory", "nomem") != "nomem",
                "wake": "、".join(cfg.get("wakeup_words") or []),
                "greeting": bool(cfg.get("enable_greeting")),
            },
            "features": feats,
            "music": _music_files(),
            "ts": time.strftime("%H:%M:%S"),
        }
    )


RESTART_ALLOWED = {"xiaozhi-esp32-server", "xiaozhi-gateway", "duck-panel"}


async def h_restart(request):
    if not authed(request):
        return web.json_response({"error": "未登录"}, status=401)
    name = request.match_info.get("name", "")
    if name not in RESTART_ALLOWED:
        return web.json_response({"error": f"不允许重启 {name}"}, status=403)
    try:
        code = await docker_restart(name)
        return web.json_response({"ok": True, "code": code})
    except Exception as exc:  # noqa: BLE001
        return web.json_response({"error": str(exc)}, status=500)


# ---------------------------------------------------------------- 小鲸（微信桥用）
MEMORY_PATH = "/data/.memory.yaml"
QUEUE_PATH = "/data/duck_cmd_queue.json"
BRIDGE_TOKEN = os.environ.get("BRIDGE_TOKEN", "duck-bridge-2026")
DEVICE_TOKEN = os.environ.get("DEVICE_CMD_TOKEN", "duck-cmd-2026")

# 远程指令：说这些词就让鸭子做动作
ACTION_WORDS = {
    "太空步": "moonwalk", "风车腿": "whirlwind_leg", "抖腿": "shake_leg", "弯腰": "bend",
    "跳舞": "dance", "舞蹈": "dance", "串烧": "showcase",
    "伸懒腰": "stretch", "打哈欠": "yawn", "困了": "sleepy",
    "摇头": "head_shake", "转头": "head_turn", "环顾": "look_around",
    "嘎嘎": "quack", "鸭叫": "quack", "叫两声": "quack", "叫一声": "quack", "咂嘴": "chatter",
    "坐下": "sit", "坐好": "sit",
    "跳一下": "jump", "跳跳": "jump", "跳个": "jump", "蹦一下": "jump",
    "走两步": "walk", "走走": "walk", "走几步": "walk", "前进": "walk",
    "转个身": "turn", "转身": "turn", "后退": "turn",
    "复位": "home", "回家": "home", "回去": "home",
    "理羽": "preen", "开心": "happy", "大笑": "laugh", "唱歌": "sing",
    "好奇": "curious", "侧头": "curious", "啄": "peck",
}


def _load_queue() -> list:
    try:
        with open(QUEUE_PATH, encoding="utf-8") as f:
            return json.load(f)
    except Exception:  # noqa: BLE001
        return []


def _save_queue(q: list) -> None:
    try:
        os.makedirs(os.path.dirname(QUEUE_PATH), exist_ok=True)
        with open(QUEUE_PATH, "w", encoding="utf-8") as f:
            json.dump(q[-200:], f, ensure_ascii=False)
    except Exception:  # noqa: BLE001
        pass


def detect_command(text: str):
    """从一句话里认出「让鸭子做什么」的意图。"""
    for word, action in ACTION_WORDS.items():
        if word not in text:
            continue
        asked = any(t in text for t in ("让", "叫", "帮", "给", "鸭子", "它", "鸭", "你"))
        short = len(text) <= 6
        if asked or short:
            return action, word
    return None, None


def _memory_brief() -> str:
    """把记忆文件压成一段给大模型看的摘要。"""
    try:
        with open(MEMORY_PATH, encoding="utf-8") as f:
            data = yaml.safe_load(f) or {}
    except Exception:  # noqa: BLE001
        return ""
    blob = None
    for v in data.values():
        if isinstance(v, str):
            try:
                blob = json.loads(v)
                break
            except Exception:  # noqa: BLE001
                continue
    if not isinstance(blob, dict):
        return ""

    parts = []
    ident = (blob.get("时空档案") or {}).get("身份图谱") or {}
    if ident:
        parts.append("【身份】" + "；".join(f"{k}: {v}" for k, v in ident.items()
                                          if not isinstance(v, list)))
        traits = ident.get("特征标记")
        if isinstance(traits, list) and traits:
            parts.append("【特征】" + "、".join(str(t) for t in traits[:5]))

    topics = (blob.get("关系网络") or {}).get("高频话题") or {}
    if isinstance(topics, dict) and topics:
        top = sorted(topics.items(), key=lambda kv: -int(kv[1] or 0))[:8]
        parts.append("【常聊】" + "、".join(str(k) for k, _ in top))

    quotes = blob.get("高光语录") or []
    if isinstance(quotes, list) and quotes:
        recent = [str(q) for q in quotes[-8:]]
        parts.append("【最近说过】" + " / ".join(recent))

    return "\n".join(parts)


async def xiaojing_reply(text: str):
    """微信里的小鲸：人设 + 记忆 + 远程指令识别。"""
    c = load_cfg()
    system = c["prompt"]
    mem = _memory_brief()
    if mem:
        system += ("\n\n[你记得的事 —— 自然用上，别像念档案]\n" + mem)
    system += ("\n\n[当前渠道] 你现在是在微信里跟我打字聊天，不是喇叭说话。"
               "回答简短口语化，1~3 句，不要用「语音」「扬声器」这类词。")

    action, word = detect_command(text)
    if action:
        q = _load_queue()
        q.append({
            "id": secrets.token_hex(6),
            "action": action,
            "text": text,
            "ts": int(time.time()),
            "state": "pending",
        })
        _save_queue(q)
        return (f"记下了——等鸭子上线我就让它「{word}」。"
                f"（队列里有 {sum(1 for x in q if x['state'] == 'pending')} 条待执行）"), action

    prov = PROVIDERS.get(c["provider"], PROVIDERS["deepseek"])
    url = prov["base"].rstrip("/") + "/chat/completions"
    payload = {
        "model": c["llm_model"] or prov["model"],
        "messages": [
            {"role": "system", "content": system},
            {"role": "user", "content": text},
        ],
        "max_tokens": 300,
        "temperature": 0.8,
    }
    async with httpx.AsyncClient(timeout=60) as cli:
        r = await cli.post(url, json=payload, headers={"Authorization": f"Bearer {c['llm_key']}"})
        j = r.json()
    if "choices" not in j:
        raise RuntimeError(json.dumps(j, ensure_ascii=False)[:300])
    return j["choices"][0]["message"]["content"].strip(), None


async def h_xiaojing(request):
    """微信桥调用：{text} -> {reply, action}"""
    body = await request.json()
    if body.get("token") != BRIDGE_TOKEN:
        return web.json_response({"error": "token 不对"}, status=403)
    text = (body.get("text") or "").strip()
    if not text:
        return web.json_response({"error": "空消息"}, status=400)
    try:
        reply, action = await xiaojing_reply(text)
        return web.json_response({"reply": reply, "action": action})
    except Exception as exc:  # noqa: BLE001
        return web.json_response({"error": f"{type(exc).__name__}: {exc}"[:300]}, status=500)


async def h_cmd_pending(request):
    """设备轮询：取待执行的远程指令。"""
    if request.query.get("token") != DEVICE_TOKEN:
        return web.json_response({"error": "token 不对"}, status=403)
    q = _load_queue()
    out = []
    changed = False
    for item in q:
        if item.get("state") == "pending":
            item["state"] = "dispatched"
            item["dispatched_at"] = int(time.time())
            changed = True
            out.append({"id": item["id"], "action": item["action"]})
    if changed:
        _save_queue(q)
    return web.json_response({"commands": out, "count": len(out)})


async def h_cmd_ack(request):
    """设备回执：某条指令执行完了。"""
    body = await request.json()
    if body.get("token") != DEVICE_TOKEN:
        return web.json_response({"error": "token 不对"}, status=403)
    cid = body.get("id")
    q = _load_queue()
    hit = False
    for item in q:
        if item.get("id") == cid:
            item["state"] = "done"
            item["done_at"] = int(time.time())
            hit = True
    if hit:
        _save_queue(q)
    return web.json_response({"ok": hit})


async def h_cmd_list(request):
    if not authed(request):
        return web.json_response({"error": "未登录"}, status=401)
    q = _load_queue()
    return web.json_response({"commands": q[-50:]})


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
    web.get("/api/overview", h_overview),
    web.post("/api/restart/{name}", h_restart),
    web.post("/api/xiaojing", h_xiaojing),
    web.get("/api/cmd/pending", h_cmd_pending),
    web.post("/api/cmd/ack", h_cmd_ack),
    web.get("/api/cmd/list", h_cmd_list),
    web.get("/favicon.ico", h_favicon),
])

if __name__ == "__main__":
    print("panel listening on 0.0.0.0:8090", flush=True)
    web.run_app(app, host="0.0.0.0", port=8090, access_log=None)
