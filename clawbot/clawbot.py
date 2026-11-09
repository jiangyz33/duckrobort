#!/usr/bin/env python3
"""微信 ClawBot 桥 —— 在微信里跟小鲸打字聊天，并下达远程指令。

协议（iLink，微信官方 AI 机器人通道）：
    扫码登录   POST /ilink/bot/get_bot_qrcode?bot_type=3
    轮询结果   GET  /ilink/bot/get_qrcode_status?qrcode=...
    收消息     POST /ilink/bot/getupdates      （长轮询）
    发消息     POST /ilink/bot/sendmessage

用法：
    python clawbot.py --login     # 首次：扫码绑定，保存 bot_token
    python clawbot.py             # 启动桥（守护进程）
    python clawbot.py --diag      # 自检
"""

import argparse
import base64
import json
import os
import sys
import time
import urllib.parse

import httpx

BASE = os.environ.get("CLAWBOT_BASE", "https://ilinkai.weixin.qq.com")
CHANNEL_VERSION = "1.0.3"
SK_ROUTE_TAG = "1001"

DATA_DIR = os.environ.get("CLAWBOT_DATA", "/data/clawbot")
TOKEN_FILE = os.path.join(DATA_DIR, "token.json")
STATE_FILE = os.path.join(DATA_DIR, "state.json")

BRAIN_URL = os.environ.get("BRAIN_URL", "http://127.0.0.1:8090/api/xiaojing")
BRIDGE_TOKEN = os.environ.get("BRIDGE_TOKEN", "duck-bridge-2026")


# ----------------------------------------------------------------- 工具
def uin_header() -> str:
    u = int.from_bytes(os.urandom(4), "big")
    return base64.b64encode(str(u).encode()).decode()


def headers(token: str = None, with_app_id: bool = False) -> dict:
    h = {
        "Content-Type": "application/json",
        "AuthorizationType": "ilink_bot_token",
        "X-WECHAT-UIN": uin_header(),
        "SKRouteTag": SK_ROUTE_TAG,
    }
    if with_app_id:
        h["iLink-App-Id"] = "bot"
    if token:
        h["Authorization"] = "Bearer " + token
    return h


def load_json(path: str, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:  # noqa: BLE001
        return default


def save_json(path: str, data) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    os.replace(tmp, path)


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


# ----------------------------------------------------------------- 登录
def login() -> bool:
    """扫码绑定一只 ClawBot，把 bot_token 存下来。"""
    try:
        r = httpx.post(
            BASE + "/ilink/bot/get_bot_qrcode?bot_type=3",
            json={"local_token_list": []},
            headers=headers(with_app_id=True),
            timeout=30,
        )
        j = r.json()
    except Exception as exc:  # noqa: BLE001
        log(f"请求二维码失败：{exc}")
        return False

    qr = j.get("qrcode")
    img = j.get("qrcode_img_content")
    if not qr:
        log(f"没拿到二维码：{json.dumps(j, ensure_ascii=False)[:300]}")
        return False

    log("请用微信扫描下面的二维码（5 分钟内有效）")
    if img:
        print()
        print("  二维码地址/内容：", img)
        print()
        if isinstance(img, str) and img.startswith(("http://", "https://")):
            # 顺手存一份 PNG，方便浏览器打开
            try:
                png = httpx.get(img, timeout=30).content
                os.makedirs(DATA_DIR, exist_ok=True)
                with open(os.path.join(DATA_DIR, "qrcode.png"), "wb") as f:
                    f.write(png)
                log(f"二维码已保存：{os.path.join(DATA_DIR, 'qrcode.png')}")
            except Exception as exc:  # noqa: BLE001
                log(f"二维码图片下载失败（不影响扫码）：{exc}")

    deadline = time.time() + 300
    verify = None
    poll_base = BASE
    while time.time() < deadline:
        time.sleep(2)
        q = {"qrcode": qr}
        if verify:
            q["verify_code"] = verify
        try:
            r = httpx.get(
                poll_base.rstrip("/") + "/ilink/bot/get_qrcode_status?" + urllib.parse.urlencode(q),
                headers=headers(with_app_id=True),
                timeout=40,
            )
            st = r.json()
        except Exception:  # noqa: BLE001
            continue

        state = st.get("status")
        if state == "confirmed":
            token = st.get("bot_token")
            if not token or ":" not in str(token):
                log("扫码成功但没拿到有效 bot_token")
                return False
            save_json(TOKEN_FILE, {
                "bot_token": token,
                "base_url": st.get("baseurl") or BASE,
                "bot_id": st.get("ilink_bot_id") or "",
            })
            log("✅ 绑定成功，bot_token 已保存")
            return True
        if state == "need_verifycode":
            verify = input("请输入微信上显示的验证码：").strip()
            log(f"已输入验证码 {verify}")
        elif state == "scaned_but_redirect":
            redirect = st.get("redirect_host") or st.get("baseurl")
            if redirect:
                poll_base = redirect if str(redirect).startswith("http") else "https://" + str(redirect)
                log(f"跳转到 {poll_base}")
        elif state in ("expired", "verify_code_blocked", "binded_redirect"):
            log(f"登录未完成：{state}")
            return False
        elif state == "scaned":
            log("已扫码，等待手机上确认…")

    log("等了 5 分钟还没完成，请重新运行 --login")
    return False


# ----------------------------------------------------------------- 收发
def extract_text(msg: dict) -> str:
    """从 item_list 里取出文字。type=1 是文本。"""
    for item in (msg.get("item_list") or []):
        if item.get("type") == 1:
            t = (item.get("text_item") or {}).get("text")
            if t:
                return str(t).strip()
    return ""


def send_text(token: str, base: str, to_user: str, ctx: str, text: str) -> bool:
    body = {
        "msg": {
            "from_user_id": "",
            "to_user_id": to_user,
            "client_id": "duck-" + os.urandom(8).hex(),
            "message_type": 2,
            "message_state": 2,
            "item_list": [{"type": 1, "text_item": {"text": text}}],
        },
        "base_info": {"channel_version": CHANNEL_VERSION},
    }
    if ctx:
        body["msg"]["context_token"] = ctx
    try:
        r = httpx.post(base + "/ilink/bot/sendmessage", json=body, headers=headers(token), timeout=25)
        j = r.json()
        ok = j.get("errcode", 0) in (0, "0", None)
        if not ok:
            log(f"发送失败：{json.dumps(j, ensure_ascii=False)[:200]}")
        return ok
    except Exception as exc:  # noqa: BLE001
        log(f"发送异常：{exc}")
        return False


def ask_brain(text: str) -> str:
    """把小鲸的回答要回来。"""
    try:
        r = httpx.post(BRAIN_URL, json={"token": BRIDGE_TOKEN, "text": text}, timeout=90)
        j = r.json()
        if j.get("reply"):
            return j["reply"]
        return "唔…我这边脑子卡了一下：" + str(j.get("error", ""))[:80]
    except Exception as exc:  # noqa: BLE001
        return f"唔…连不上脑子了（{type(exc).__name__}）"


def run() -> int:
    cfg = load_json(TOKEN_FILE, {})
    token = cfg.get("bot_token")
    if not token:
        log("还没绑定，先运行：python clawbot.py --login")
        return 1
    base = cfg.get("base_url") or BASE
    state = load_json(STATE_FILE, {"cursor": "", "seen": []})
    log(f"桥已启动（bot_id={cfg.get('bot_id','?')}, 大脑={BRAIN_URL}）")

    while True:
        try:
            body = {
                "get_updates_buf": state.get("cursor", ""),
                "base_info": {"channel_version": CHANNEL_VERSION},
            }
            r = httpx.post(base + "/ilink/bot/getupdates", json=body,
                           headers=headers(token), timeout=90)
            j = r.json()
        except Exception as exc:  # noqa: BLE001
            log(f"轮询异常：{type(exc).__name__}: {exc}")
            time.sleep(3)
            continue

        if j.get("errcode") not in (0, "0", None):
            log(f"getupdates 返回错误：{json.dumps(j, ensure_ascii=False)[:200]}")
            time.sleep(3)
            continue

        if j.get("get_updates_buf"):
            state["cursor"] = j["get_updates_buf"]

        for msg in (j.get("msgs") or []):
            uid = msg.get("from_user_id")
            ctx = msg.get("context_token")
            mid = msg.get("msg_id") or msg.get("message_id") or msg.get("client_id") or ""
            if not uid or not ctx:
                continue
            seen = state.setdefault("seen", [])
            if mid and mid in seen:
                continue
            if mid:
                seen.append(mid)
                del seen[:-200]

            text = extract_text(msg)
            if not text:
                continue
            log(f"收到 @{uid[:8]}：{text[:60]}")
            reply = ask_brain(text)
            log(f"回复：{reply[:60]}")
            send_text(token, base, uid, ctx, reply)

        save_json(STATE_FILE, state)


# ----------------------------------------------------------------- 自检
def diag() -> int:
    cfg = load_json(TOKEN_FILE, {})
    print(f"数据目录   : {DATA_DIR}")
    print(f"token 文件 : {TOKEN_FILE}  ({'有' if cfg.get('bot_token') else '没有'})")
    if cfg.get("bot_token"):
        tok = cfg["bot_token"]
        head = tok.split(":")[0]
        print(f"bot_token  : {head}:{'*' * 12}（长度 {len(tok)}）")
        print(f"base_url   : {cfg.get('base_url', BASE)}")
    print(f"大脑地址   : {BRAIN_URL}")

    try:
        r = httpx.post(BRAIN_URL, json={"token": BRIDGE_TOKEN, "text": "自检一下，你是谁？"}, timeout=60)
        d = r.json()
        print(f"大脑连通   : {'✅' if d.get('reply') else '❌'}  {str(d.get('reply') or d.get('error'))[:80]}")
    except Exception as exc:  # noqa: BLE001
        print(f"大脑连通   : ❌ {type(exc).__name__}: {exc}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="微信 ClawBot 桥")
    ap.add_argument("--login", action="store_true", help="扫码绑定")
    ap.add_argument("--diag", action="store_true", help="自检")
    args = ap.parse_args()

    if args.login:
        return 0 if login() else 2
    if args.diag:
        return diag()
    return run()


if __name__ == "__main__":
    sys.exit(main())
