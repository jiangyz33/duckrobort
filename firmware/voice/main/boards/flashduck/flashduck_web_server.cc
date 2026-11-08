#include "flashduck_web_server.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "flashduck_controller.h"
#include "wifi_manager.h"

#define TAG "FlashduckWeb"

namespace {

constexpr int kDefaultSpeed = 700;
constexpr int kDefaultHipOffset = 0;
constexpr int kDefaultHipAmp = 30;
constexpr int kDefaultFootAmp = 30;
constexpr int kDefaultPhase = 90;

const char kHtml[] = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>Flashduck 步态调参</title>
<style>
  :root {
    --bg: #0b1220;
    --card: #141f31;
    --card2: #1b2a40;
    --text: #edf3fb;
    --muted: #93a6bf;
    --accent: #ffb300;
    --accent2: #ffd166;
    --line: rgba(255,255,255,.10);
    --good: #37d67a;
    --danger: #ff5c6c;
  }
  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  body {
    margin: 0; padding: 18px 14px 40px;
    background: radial-gradient(1200px 600px at 50% -10%, #1a2b44 0%, var(--bg) 60%);
    color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", "PingFang SC", "Microsoft YaHei", sans-serif;
  }
  .wrap { max-width: 720px; margin: 0 auto; }
  header { padding: 6px 4px 16px; }
  h1 { margin: 0; font-size: 24px; letter-spacing: .5px; }
  h1 span { color: var(--accent); }
  .sub { color: var(--muted); margin-top: 8px; font-size: 13px; line-height: 1.6; }
  .card {
    background: linear-gradient(180deg, var(--card2), var(--card));
    border: 1px solid var(--line); border-radius: 18px;
    padding: 16px 16px 12px; margin: 12px 0;
    box-shadow: 0 10px 30px rgba(0,0,0,.25);
  }
  .row { display: flex; justify-content: space-between; align-items: baseline; gap: 12px; }
  .label { font-size: 16px; font-weight: 650; }
  .value { color: var(--accent); font-size: 18px; font-weight: 750; font-variant-numeric: tabular-nums; }
  .hint { color: var(--muted); font-size: 12.5px; line-height: 1.55; margin: 6px 0 10px; }
  input[type=range] {
    -webkit-appearance: none; appearance: none;
    width: 100%; height: 38px; background: transparent; margin: 0; padding: 0;
  }
  input[type=range]::-webkit-slider-runnable-track {
    height: 10px; border-radius: 99px;
    background: linear-gradient(90deg, var(--accent) 0%, var(--accent) var(--pct, 50%), #26364e var(--pct, 50%), #26364e 100%);
    border: 1px solid rgba(255,255,255,.08);
  }
  input[type=range]::-webkit-slider-thumb {
    -webkit-appearance: none; width: 30px; height: 30px; border-radius: 50%;
    background: #fff; border: 5px solid var(--accent); margin-top: -11px;
    box-shadow: 0 3px 12px rgba(0,0,0,.45);
  }
  input[type=range]::-moz-range-track { height: 10px; border-radius: 99px; background: #26364e; }
  input[type=range]::-moz-range-progress { height: 10px; border-radius: 99px; background: var(--accent); }
  input[type=range]::-moz-range-thumb { width: 22px; height: 22px; border-radius: 50%; background: #fff; border: 5px solid var(--accent); }
  .actions { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 16px; }
  button {
    border: 0; border-radius: 14px; padding: 15px 12px; font-size: 15px; font-weight: 750;
    color: #10151f; background: var(--accent); cursor: pointer; transition: transform .06s ease, opacity .15s ease;
  }
  button:active { transform: scale(.985); }
  button:disabled { opacity: .35; cursor: not-allowed; }
  button.secondary { background: #6ea8ff; color: #07101d; }
  button.danger { background: var(--danger); color: #fff; }
  button.ghost { background: #223149; color: var(--text); border: 1px solid var(--line); }
  button.save { width: 100%; margin-top: 16px; background: var(--good); color: #06150b; }
  .status {
    margin-top: 14px; padding: 12px 14px; border-radius: 14px;
    background: rgba(255,255,255,.04); border: 1px solid var(--line);
    color: var(--muted); font-size: 13px; line-height: 1.5;
  }
  .status b { color: var(--good); }
  .summary { color: var(--muted); font-size: 12px; margin-top: 10px; line-height: 1.6; }
  details { margin-top: 16px; color: var(--muted); font-size: 13px; line-height: 1.65; }
  details summary { color: var(--text); font-weight: 650; cursor: pointer; padding: 8px 0; }
  .note { color: var(--muted); font-size: 12px; margin: 10px 4px 0; line-height: 1.6; }
  @media (max-width: 380px) { .actions { grid-template-columns: 1fr; } }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>Flashduck <span>步态调参</span></h1>
    <div class="sub">拖动滑块，点“向前走 5 步”立即测试。参数只影响下一次测试，不会打断正在执行的动作。</div>
  </header>

  <div class="card">
    <div class="row"><div class="label">双脚内收</div><div class="value"><span id="hip_offset_v">25</span>°</div></div>
    <div class="hint">让两只脚往身体中间收，收窄支撑面。当前推荐 25°（本版上限）；脚碰脚就减小。</div>
    <input id="hip_offset" type="range" min="0" max="25" step="1" value="25">
  </div>

  <div class="card">
    <div class="row"><div class="label">身体左右摆幅</div><div class="value"><span id="hip_amp_v">44</span>°</div></div>
    <div class="hint">髋部左右换重心的幅度。摆动脚抬不起来就加大；身体晃太厉害/舵机吃力就减小。</div>
    <input id="hip_amp" type="range" min="15" max="50" step="1" value="44">
  </div>

  <div class="card">
    <div class="row"><div class="label">脚踝摆幅 / 步幅</div><div class="value"><span id="foot_amp_v">34</span>°</div></div>
    <div class="hint">决定一步迈多大。脚打滑、拍地、顶限位就减小。</div>
    <input id="foot_amp" type="range" min="15" max="50" step="1" value="34">
  </div>

  <div class="card">
    <div class="row"><div class="label">抬脚时序</div><div class="value"><span id="phase_deg_v">108</span>°</div></div>
    <div class="hint">90° 是官方套件默认。宽站距可试 100~110°，让脚先抬起再往前推。</div>
    <input id="phase_deg" type="range" min="60" max="120" step="1" value="108">
  </div>

  <div class="card">
    <div class="row"><div class="label">每一步时间</div><div class="value"><span id="speed_v">600</span> ms</div></div>
    <div class="hint">数值越大越慢。当前推荐 600ms；如果舵机发热、抖动或低电量掉步，再放慢到 700~800ms。</div>
    <input id="speed" type="range" min="400" max="2000" step="50" value="600">
  </div>

  <div class="card">
    <div class="row"><div class="label">走路时脖子摆动</div><div class="value"><span id="head_swing_v">24</span>°</div></div>
    <div class="hint">0 表示走路时不摆头；想让鸭子边走边看可以加到 10~25°。</div>
    <input id="head_swing" type="range" min="0" max="90" step="1" value="24">
  </div>

  <button class="save" onclick="saveDefaults()">保存为默认（写入机器人）</button>

  <div class="actions">
    <button id="forward" onclick="testWalk(1)">向前走 5 步</button>
    <button id="backward" class="secondary" onclick="testWalk(-1)">向后走 5 步</button>
    <button id="stop" class="danger" onclick="stopWalk()">停止 / 回正</button>
    <button class="ghost" onclick="resetDefaults()">恢复官方默认</button>
  </div>

  <div id="status" class="status">正在读取机器人保存的默认参数…</div>
  <div id="summary" class="summary"></div>

  <details>
    <summary>每个参数到底在调什么？</summary>
    <p><b>双脚内收</b>：把脚往身体中间收。腿间距越宽，越需要一点内收，否则两只脚都吃地，只能蹭。</p>
    <p><b>身体左右摆幅</b>：让身体把重心移到支撑脚上方。抬不起脚就加，晃得太厉害就减。</p>
    <p><b>脚踝摆幅</b>：真正决定步幅。太大反而打滑，先保证脚能干净离地。</p>
    <p><b>抬脚时序</b>：髋和脚的配合时机。90° 是原版，100~110° 往往更适合宽站距。</p>
    <p><b>每一步时间</b>：一步走多久。宽站距需要更多换重心时间，所以先慢后快。</p>
    <p><b>脖子摆动</b>：只影响走路时的头部动作，不影响腿。</p>
  </details>
  <div class="note">这是局域网页面，仅在机器人当前 WiFi 内可访问。调参不会写入固件，刷新页面会保留上次拖动值。</div>
</div>

<script>
const FALLBACK = { hip_offset: 15, hip_amp: 20, foot_amp: 25, phase_deg: 75, speed: 800, head_swing: 15 };
const ORIGINAL = { hip_offset: 0, hip_amp: 30, foot_amp: 30, phase_deg: 90, speed: 700, head_swing: 0 };
const KEYS = Object.keys(FALLBACK);
let busy = false;

function $(id) { return document.getElementById(id); }

function readParams() {
  const p = {};
  for (const k of KEYS) p[k] = Number($(k).value);
  return p;
}

function applyParams(p) {
  for (const k of KEYS) if (p[k] !== undefined) $(k).value = p[k];
  paint();
}

function paint() {
  const p = readParams();
  for (const k of KEYS) {
    const el = $(k);
    $(k + '_v').textContent = p[k];
    const pct = ((Number(el.value) - Number(el.min)) / (Number(el.max) - Number(el.min))) * 100;
    el.style.setProperty('--pct', pct + '%');
  }
  $('summary').textContent =
    `当前参数：内收 ${p.hip_offset}° · 摆幅 ${p.hip_amp}° · 步幅 ${p.foot_amp}° · 时序 ${p.phase_deg}° · 周期 ${p.speed}ms · 摆头 ${p.head_swing}°`;
}

function setButtons(enabled) {
  for (const id of ['forward', 'backward']) $(id).disabled = !enabled;
  $('stop').disabled = false;
}

function setStatus(text) { $('status').innerHTML = text; }

async function loadDefaults() {
  try {
    const res = await fetch('/api/gait', { cache: 'no-store' });
    const data = await res.json();
    if (data.ok && data.gait) {
      applyParams(data.gait);
      setStatus('<b>已载入机器人保存的默认参数。</b>可以直接测试，或修改后点“保存为默认”。');
    } else {
      applyParams(FALLBACK);
      setStatus('<b>机器人尚未返回默认参数，先用推荐值。</b>');
    }
  } catch (e) {
    applyParams(FALLBACK);
    setStatus('<b>未读取到默认参数，先用推荐值。</b>检查手机是否还连着机器人 WiFi。');
  }
}

async function testWalk(direction) {
  if (busy) return;
  const p = readParams();
  busy = true;
  setButtons(false);
  setStatus(`<b>正在${direction > 0 ? '向前' : '向后'}走 5 步…</b> 周期 ${p.speed}ms · 内收 ${p.hip_offset}° · 摆幅 ${p.hip_amp}° · 步幅 ${p.foot_amp}°`);
  try {
    const res = await fetch('/api/walk', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(Object.assign({}, p, { direction: direction, steps: 5 }))
    });
    const data = await res.json();
    if (!data.ok) throw new Error(data.error || 'robot rejected');
    setStatus(`<b>已发送：${direction > 0 ? '前进' : '后退'} 5 步</b>。观察脚是否离地、是否打滑；走完后可以马上改参数再试。`);
  } catch (e) {
    setStatus('<b>发送失败：</b>' + e.message + '。检查页面是否仍连着机器人 WiFi。');
  }
  setTimeout(() => { busy = false; setButtons(true); pollStatus(); }, Math.max(3500, p.speed * 5 + 2500));
}

async function stopWalk() {
  try {
    await fetch('/api/stop', { method: 'POST' });
    setStatus('<b>已发送停止/回正。</b>');
    busy = false;
    setButtons(true);
  } catch (e) {
    setStatus('<b>停止失败：</b>' + e.message);
  }
}

async function saveDefaults() {
  const p = readParams();
  setStatus('正在写入机器人 NVS…');
  try {
    const res = await fetch('/api/save', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(p)
    });
    const data = await res.json();
    if (!data.ok) throw new Error(data.error || 'save failed');
    if (data.gait) applyParams(data.gait);
    setStatus('<b>已保存为默认。</b>语音 walk 和下次打开页面都会使用这组参数。');
  } catch (e) {
    setStatus('<b>保存失败：</b>' + e.message);
  }
}

function resetDefaults() {
  applyParams(ORIGINAL);
  setStatus('已填入官方默认参数；如果要永久使用，点“保存为默认”。');
}

async function pollStatus() {
  try {
    const res = await fetch('/api/status', { cache: 'no-store' });
    const data = await res.json();
    if (!data.moving && busy) {
      busy = false;
      setButtons(true);
      setStatus('<b>动作完成。</b>可以调整参数，再点测试走 5 步。');
    }
  } catch (e) {}
}

for (const k of KEYS) {
  $(k).addEventListener('input', paint);
}
loadDefaults();
setInterval(pollStatus, 1200);
</script>
</body>
</html>)HTML";

esp_err_t SendJson(httpd_req_t* req, const char* json) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t SendError(httpd_req_t* req, const char* error) {
    char body[160];
    std::snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", error);
    httpd_resp_set_status(req, "400 Bad Request");
    return SendJson(req, body);
}

int JsonInt(const cJSON* root, const char* key, int fallback) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

esp_err_t RootHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WalkHandler(httpd_req_t* req) {
    char body[512];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        return SendError(req, "empty body");
    }
    body[len] = '\0';

    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) {
        return SendError(req, "invalid json");
    }

    const auto defaults = FlashduckGetGaitDefaults();
    int direction = JsonInt(root, "direction", 1);
    int steps = JsonInt(root, "steps", 5);
    int speed = JsonInt(root, "speed", defaults.speed);
    int hip_offset = JsonInt(root, "hip_offset", defaults.hip_offset);
    int hip_amp = JsonInt(root, "hip_amp", defaults.hip_amp);
    int foot_amp = JsonInt(root, "foot_amp", defaults.foot_amp);
    int phase_deg = JsonInt(root, "phase_deg", defaults.phase_deg);
    int head_swing = JsonInt(root, "head_swing", defaults.head_swing);
    cJSON_Delete(root);

    FlashduckStartWalk(steps, speed, direction, hip_offset, hip_amp, foot_amp, phase_deg,
                       head_swing);

    char response[128];
    std::snprintf(response, sizeof(response),
                  "{\"ok\":true,\"steps\":%d,\"direction\":%d}", steps, direction);
    return SendJson(req, response);
}

esp_err_t StopHandler(httpd_req_t* req) {
    FlashduckStop();
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t StatusHandler(httpd_req_t* req) {
    return SendJson(req, FlashduckIsMoving() ? "{\"moving\":true}" : "{\"moving\":false}");
}

void GaitToJson(char* out, size_t out_size, const FlashduckGaitParams& gait) {
    std::snprintf(out, out_size,
                  "{\"ok\":true,\"gait\":{\"hip_offset\":%d,\"hip_amp\":%d,\"foot_amp\":%d,"
                  "\"phase_deg\":%d,\"speed\":%d,\"head_swing\":%d}}",
                  gait.hip_offset, gait.hip_amp, gait.foot_amp, gait.phase_deg, gait.speed,
                  gait.head_swing);
}

esp_err_t GaitHandler(httpd_req_t* req) {
    char response[256];
    GaitToJson(response, sizeof(response), FlashduckGetGaitDefaults());
    return SendJson(req, response);
}

esp_err_t SaveHandler(httpd_req_t* req) {
    char body[512];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        return SendError(req, "empty body");
    }
    body[len] = '\0';

    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) {
        return SendError(req, "invalid json");
    }

    FlashduckGaitParams gait = FlashduckGetGaitDefaults();
    gait.hip_offset = JsonInt(root, "hip_offset", gait.hip_offset);
    gait.hip_amp = JsonInt(root, "hip_amp", gait.hip_amp);
    gait.foot_amp = JsonInt(root, "foot_amp", gait.foot_amp);
    gait.phase_deg = JsonInt(root, "phase_deg", gait.phase_deg);
    gait.speed = JsonInt(root, "speed", gait.speed);
    gait.head_swing = JsonInt(root, "head_swing", gait.head_swing);
    cJSON_Delete(root);

    FlashduckSaveGaitDefaults(gait);

    char response[256];
    GaitToJson(response, sizeof(response), FlashduckGetGaitDefaults());
    return SendJson(req, response);
}

}  // namespace

bool FlashduckWebServer::Start() {
    if (server_ != nullptr) {
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.stack_size = 8192;
    config.server_port = 80;

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "端口 80 启动失败 (%s)，改用 8080", esp_err_to_name(err));
        config.server_port = 8080;
        err = httpd_start(&server_, &config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "调参页面 HTTP 服务启动失败: %s", esp_err_to_name(err));
        server_ = nullptr;
        return false;
    }
    port_ = config.server_port;

    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = RootHandler;
    root.user_ctx = this;

    httpd_uri_t walk = {};
    walk.uri = "/api/walk";
    walk.method = HTTP_POST;
    walk.handler = WalkHandler;
    walk.user_ctx = this;

    httpd_uri_t stop = {};
    stop.uri = "/api/stop";
    stop.method = HTTP_POST;
    stop.handler = StopHandler;
    stop.user_ctx = this;

    httpd_uri_t status = {};
    status.uri = "/api/status";
    status.method = HTTP_GET;
    status.handler = StatusHandler;
    status.user_ctx = this;

    httpd_uri_t gait = {};
    gait.uri = "/api/gait";
    gait.method = HTTP_GET;
    gait.handler = GaitHandler;
    gait.user_ctx = this;

    httpd_uri_t save = {};
    save.uri = "/api/save";
    save.method = HTTP_POST;
    save.handler = SaveHandler;
    save.user_ctx = this;

    if (httpd_register_uri_handler(server_, &root) != ESP_OK ||
        httpd_register_uri_handler(server_, &walk) != ESP_OK ||
        httpd_register_uri_handler(server_, &stop) != ESP_OK ||
        httpd_register_uri_handler(server_, &status) != ESP_OK ||
        httpd_register_uri_handler(server_, &gait) != ESP_OK ||
        httpd_register_uri_handler(server_, &save) != ESP_OK) {
        ESP_LOGE(TAG, "调参页面 URI 注册失败");
        httpd_stop(server_);
        server_ = nullptr;
        return false;
    }

    std::string ip = WifiManager::GetInstance().GetIpAddress();
    if (ip.empty()) {
        ESP_LOGW(TAG, "调参页面已启动，但尚未获取 IP；端口 %d", port_);
    } else {
        ESP_LOGI(TAG, "Flashduck 调参页面: http://%s%s/", ip.c_str(),
                 port_ == 80 ? "" : ":8080");
    }
    return true;
}
