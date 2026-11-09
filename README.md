# 🦆 DuckRobort

> 一只住在桌面上的机器鸭 —— 会说爱弥斯的声音、记得你聊过的事、还能给你放歌。
> 基于开源项目 [Flashduck](https://github.com/FlashCat-Jordan/Flashduck) 的二次开发。

<p align="center">
  <a href="https://github.com/jiangyz33/duckrobort/releases/latest"><img alt="Firmware" src="https://img.shields.io/github/v/release/jiangyz33/duckrobort?label=firmware&style=flat-square&color=ffb703"></a>
  <img alt="MCU: ESP32-S3" src="https://img.shields.io/badge/MCU-ESP32--S3-1f6feb?style=flat-square">
  <img alt="ESP-IDF 6.0.1+" src="https://img.shields.io/badge/ESP--IDF-6.0.1%2B-black?style=flat-square">
  <img alt="6 servos" src="https://img.shields.io/badge/Servos-6-orange?style=flat-square">
  <img alt="License: MIT" src="https://img.shields.io/badge/License-MIT-2ea44f?style=flat-square">
</p>

---

## 这是什么

一只 **ESP32-S3 桌面机器鸭**：六舵机身体、单眼动画屏幕、可选摄像头、语音对话。

这个仓库是在原版 Flashduck 固件基础上做的个人改造版，**不是官方发行版**。硬件、外壳、舵机结构完全沿用原项目，改动集中在固件行为和服务端配置上。

**你可以把它当成一份「怎么把开源桌宠改出自己的味道」的参考实现**：33 个可调动作、会随情绪变色的眼睛、能自己开口的定时提醒、以及一个零样本复刻的角色音色。

---

## 和原版有什么不同

| 维度 | 原版 Flashduck | 这个版本 |
|---|---|---|
| **人格** | 通用助手人设 | 「小鲸」DeepSeek 鲸鱼娘（鲸鱼灵魂 + 鸭子身体） |
| **音色** | Edge TTS 晓晓 | **《鸣潮》爱弥斯零样本复刻**（硅基流动 CosyVoice2） |
| **动作** | 20 个可调动作 | **33 个**（补全 talk/laugh/sing/think/happy/moonwalk 等） |
| **说话** | 静态表情 | **嘴部随机开合**，跟随 TTS 节奏 |
| **眼睛** | 5 种状态、单一高光 | **10 种神态 + 双高光绕行 + 瞳孔呼吸 + 睫毛 + 情绪联动** |
| **定时提醒** | ❌ 无 | ✅ **NVS 持久化 + 到点自己开口**（单次/每天/工作日） |
| **待办清单** | ❌ 无 | ✅ **语音记事 + 到点催办 + 问就报**（可标记完成） |
| **推送** | 忙时直接丢弃 | **排队**，回到空闲自动补播 |
| **联网自愈** | 拿不到 IP 时卡死 | **看门狗**：50s 无 IP → 自动重启 ×2 → 进配网 |
| **OTA 重试** | 无上限，最坏卡 85 分钟 | 封顶 60s，最坏约 8 分钟自动回待机 |
| **动作队列** | 满了静默丢弃 | **返回错误**，让模型知道没执行 |
| **天气** | 需要和风 API key | **免 key**（Open-Meteo 自定义插件） |
| **管理** | 只有局域网调参页 | 额外自建 **Web 控制面板**（人设/模型/音色/技能/日志） |

---

## 硬件

| 项 | 规格 |
|---|---|
| MCU | ESP32-S3，16 MB Flash，8 MB PSRAM |
| 舵机 | 6 × MG90 类离合舵机（双腿、双脚、脖子、嘴巴） |
| 屏幕 | ST7789 240×240（SPI） |
| 摄像头 | 可选，OV2640 / OV3660 / GC2145 自动识别 |
| 音频 | I2S 麦克风 + 扬声器 |

**舵机接线（摄像头版）**

| 关节 | GPIO | 限位 |
|---|---:|---|
| 左腿 | 5 | 0–180 |
| 右腿 | 43 | 0–180 |
| 左脚 | 6 | 0–180 |
| 右脚 | 44 | 0–180 |
| 嘴巴 | 4 | **45–60°** ⚠️ |
| 脖子 | 7 | **45–180°**（135° 为正前） |

> ⚠️ 嘴巴超过 60° 会顶坏机械结构。固件在 `Oscillator::Write()` 最终输出处做了硬钳位，任何代码路径都越不过。

---

## 系统架构

```
┌──────────────────────────┐        ┌──────────────────────────────┐
│  Flashduck 固件 (ESP32-S3)│        │  xiaozhi-esp32-server (Docker)│
│                          │        │                              │
│  · 6 路 LEDC 舵机驱动      │  WS    │  · ASR: sherpa-onnx SenseVoice│
│  · ESP-SR 唤醒词「小鸭小鸭」│◄──────►│  · LLM: DeepSeek             │
│  · LVGL 单眼动画           │        │  · TTS: 硅基流动 CosyVoice2   │
│  · 定时提醒（NVS）         │        │  · VAD: Silero                │
│  · 摄像头拍照上传          │        │  · 视觉: 智谱 glm-4v-flash    │
│                          │        │  · 搜索: Tavily               │
└──────────────────────────┘        └──────────────────────────────┘
                                              ▲
                                              │ 80 端口反代
                                    ┌─────────┴─────────┐
                                    │  nginx gateway    │
                                    │  + 自建控制面板    │
                                    └───────────────────┘
```

---

## 快速开始

### 0. 或者直接用现成固件

不想自己编译的话，[**下载 Release 里的固件**](https://github.com/jiangyz33/duckrobort/releases/latest) 直接烧：

| 文件 | 偏移 | 用途 |
|---|---|---|
| `merged-binary.bin` | `0x0` | 整包，首次刷机 |
| `xiaozhi.bin` | `0x20000` | 仅应用，保留 WiFi / 步态 / 待办 |
| `generated_assets.bin` | `0x800000` | 只更新 assets 分区 |

```bash
esptool --chip esp32s3 -p COM3 -b 460800 \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 merged-binary.bin
```

附件里还有 `FLASH.md`（详细步骤）和 `SHA256SUMS.txt`（校验和）。

> 📦 当前版本：**[fw-v1.0.0](https://github.com/jiangyz33/duckrobort/releases/tag/fw-v1.0.0)** —— 双眼 Kawaii 动画、唇形同步、33 个动作、待办/计时/晨报。
> 固件二进制**不放在 git 历史里**（每次 2.6 MB 会把仓库撑大），统一走 Release。

### 1. 编译固件

需要 **ESP-IDF 6.0.1+**（推荐 `release/v6.1`，**不支持 5.x**）。

```bash
cd firmware/voice
python scripts/build.py flashduck --name flashduck --language zh-CN \
  --wake-word wn9_xiaoyaxiaoya_tts2
```

产物：`build/merged-binary.bin`（整包）、`build/xiaozhi.bin`（app）

> **路径必须是纯 ASCII**，中文路径会报 `picolibc.specs` 错误。

### 2. 烧录

```bash
# 全刷（含唤醒词资源，会清空 NVS / WiFi 配置）
esptool --chip esp32s3 -p COM3 -b 460800 write-flash 0x0 build/merged-binary.bin

# 只升级程序（保留 WiFi、步态参数、定时提醒）
esptool --chip esp32s3 -p COM3 -b 460800 write-flash 0x20000 build/xiaozhi.bin
```

### 3. 配网

首次开机自动进配网热点 `Xiaozhi-XXXX` → 手机连上打开 `192.168.4.1` → 填 WiFi。

**OTA 地址二选一**：

- 配网页面的「自定义 OTA 地址」填 `http://你的服务器/xiaozhi/ota/`
- 或改 `firmware/voice/main/boards/flashduck/config.json` 里的 `CONFIG_OTA_URL` 重新编译

### 4. 服务器

需要一台能跑 Docker 的机器（1 GB 内存起步）。

```yaml
services:
  xiaozhi-esp32-server:
    image: ghcr.nju.edu.cn/xinnan-tech/xiaozhi-esp32-server:server_latest
    container_name: xiaozhi-esp32-server
    restart: always
    security_opt: [seccomp:unconfined]
    ports: ["127.0.0.1:8000:8000", "127.0.0.1:8003:8003"]
    volumes:
      - ./data:/opt/xiaozhi-esp32-server/data
      - ./models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17:/opt/xiaozhi-esp32-server/models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17:ro
      - ./plugins/weather_now.py:/opt/xiaozhi-esp32-server/plugins_func/functions/weather_now.py:ro
```

`data/.config.yaml` 最小配置：

```yaml
server:
  websocket: ws://你的服务器/xiaozhi/v1/
  vision_explain: http://你的服务器/mcp/vision/explain
  timezone_offset: 8

selected_module:
  ASR: SherpaASR      # 轻量本地识别，~230MB 模型 / ~600MB 内存
  LLM: DeepSeekLLM
  VLLM: ChatGLMVLLM
  TTS: SiliconFlowTTS
  Intent: function_call

LLM:
  DeepSeekLLM:
    type: openai
    model_name: deepseek-chat
    base_url: https://api.deepseek.com
    api_key: "<你的 DeepSeek Key>"

TTS:
  SiliconFlowTTS:
    type: siliconflow
    model: FunAudioLLM/CosyVoice2-0.5B
    voice: "speech:<你的复刻音色 ID>"
    access_token: "<你的硅基流动 Key>"

VLLM:
  ChatGLMVLLM:
    type: openai
    model_name: glm-4v-flash
    url: https://open.bigmodel.cn/api/paas/v4/
    api_key: "<你的智谱 Key>"

plugins:
  web_search:
    provider: tavily
    api_key: "<你的 Tavily Key>"
    max_results: 5
```

> **本仓库不含任何 API Key。** 所有密钥都在服务器端的 `data/.config.yaml`（权限 600），不要提交。

---

## 功能详解

### 动作系统（33 个）

模型通过 `self.flashduck.action` 调用，参数：`action / steps / speed / direction / amount`。

| 类别 | 动作 |
|---|---|
| **移动** | `walk` `turn` `jump` `swing` `updown` `sit` `home` `moonwalk` `bend` `shake_leg` `whirlwind_leg` `showcase` |
| **脖子** | `head_turn` `head_shake` `look_around` |
| **嘴与叫声** | `quack`（带爱弥斯音效）`chatter` `mouth_open` `mouth_close` `mouth_move` `yawn` `sleepy` |
| **说话/情绪** | `talk` `laugh` `sing` `think` `happy` |
| **组合** | `curious` `preen` `happy_waddle` `stretch` `dance` `peck` |

底层是振荡器驱动的六个关节：

```
LEDC 50Hz → 500~2500µs ↔ -90~90°
三层保护：SetRange 硬限位 / 700°/s 速度限制 / RequestStop 协作中断
```

**说话嘴型**：进入 Speaking 状态后，动作任务提速到 60ms 轮询，驱动嘴巴在 **45~55°** 之间随机开合，节奏 50~130ms。

### 眼睛动画

单眼由 LVGL 纯代码绘制（无 GIF 素材），10 种神态：

| 神态 | 视觉 | 触发 |
|---|---|---|
| Idle | 琥珀色、半眯、慢速扫视 | 待机 |
| Scanning | 冷青、瞳孔缩小、左右扫 | 连网中 |
| Connected | 绿、月牙眯眼（2.2s） | 刚连上 |
| Listening | 紫罗兰、睁到最大 | 聆听 |
| Speaking | 橙色、活跃扫视 | 说话 |
| Happy / Curious / Sleepy / Angry / Sad | 各自配色与眼睑 | 服务端 emoji 情绪 |

动效细节：

- **双高光**绕小圆反相旋转，大小随正弦呼吸
- **瞳孔**按正弦轻微胀缩（2%~8%）
- **睫毛** 3 根，只在上眼睑压低时露出（眨眼/困倦/生气）
- 所有参数统一用 8%/帧 lerp 过渡，不硬切

> 情绪来源：服务端扫描回复文本里的 emoji（`😂→funny`、`😔→sad`…），映射成情绪名下发。人设提示词里引导模型每句带一个表情即可。

### 定时提醒

模型调 `self.schedule.add(time="08:00", text="吃药", repeat=1)` 设置，**存在 NVS，掉电不丢**。

到点时如果设备空闲：

```
打开 WebSocket → 发送「【定时提醒】08:00，提醒主人：吃药。」
→ 服务端当成"用户说的话" → LLM 用人设生成 → TTS 念出来
```

| 项 | 说明 |
|---|---|
| 上限 | 16 条 |
| 重复 | `0` 单次（响完自删）/ `1` 每天 / `2` 工作日 |
| 补播窗口 | 到点后 5 分钟内都算（设备忙完能补上） |
| 时间来源 | 开机从 OTA 响应的 `server_time` 同步（东八区），没同步不触发 |
| 冲突处理 | 设备忙则跳过本次 tick，仍在窗口内下次再试 |

### 待办清单

比「定时提醒」更接近记事本：**内容和时间都可以不写**，到点会催，问它也会报。

```
你：帮我记一下，明天下午三点要去取快递
它：记下了：「去取快递」，明天 15:00 提醒你，编号 3
（第二天 15:00，它自己开口）
它：主人，该去取快递啦，要不要我标记完成？😄
你：做完了
它：好，第 3 条已标记完成
```

**三个核心操作**：

| 工具 | 什么时候用 | 参数 |
|---|---|---|
| `self.todo.add` | 「帮我记一下」「别忘了」 | `text` 事情，`time` HH:MM（可空），`day_offset` 0今天/1明天/2后天/-1只记事 |
| `self.todo.list` | 「我还有什么要做的」 | — |
| `self.todo.done` | 「做完了」「划掉第 3 条」 | `id` |

`self.todo.remove` 用于记错了要撤销。

**设计细节**：

- **不依赖模型知道今天是几号**——模型只传 `day_offset`，日期由设备自己按本地时钟算
- 说的时刻**今天已经过了**时，自动顺延到明天，并在回复里说明
- 到点提醒后**不会自动删除**，要等用户确认完成；逾期的会在列表里标「已过期」
- 列表按「日期 → 时间」排序，没定时间的排在最后
- 与定时提醒共用一份 NVS 存储（上限 32 条）和同一个轮询任务



### 视觉

设备拍照 → POST 到 `/mcp/vision/explain` → 智谱 `glm-4v-flash` 识别 → 语音回答。

问「我手里拿的是什么」即可。

### 联网自愈

```
开机 50 秒内没拿到 IP → 自动重启（最多 2 次）
第 3 次仍失败 → 进配网模式
一旦联网成功或已进配网 → 计数清零
```

解决路由器 DHCP 不给地址时设备「假死」的问题。

---

## 固件改动清单

相对原版新增/修改的文件：

| 文件 | 改动 |
|---|---|
| `main/boards/flashduck/schedule_manager.{h,cc}` | **新增** 定时提醒：NVS 持久化 + 轮询任务 + 3 个 MCP 工具 |
| `main/boards/flashduck/flashduck_display.{h,cc}` | 眼睛动画重写：10 神态、双高光、睫毛、瞳孔呼吸、情绪映射 |
| `main/boards/flashduck/flashduck_controller.cc` | 动作从 20 → 33、队列满报错、说话嘴型、quack 音效、定时提醒挂载 |
| `main/boards/flashduck/flashduck_robot.cc` | **联网看门狗** |
| `main/assets/common/quack.ogg` | **新增** 爱弥斯音色合成的「嘎嘎」 |
| `main/application.{h,cc}` | 主动播报 `SpeakProactive()`、notify 排队补播 |
| `main/device_state_machine.cc` | `FatalError → Idle` 逃生通道 |
| `main/boards/flashduck/config.json` | 板卡编译参数（OTA 地址留了占位符） |

---

## 服务端插件

`plugins/weather_now.py` —— 免 API Key 的天气插件，基于 [Open-Meteo](https://open-meteo.com/)：

```bash
cp plugins/weather_now.py <server>/xiaozhi-server/plugins/weather_now.py
```

在 `Intent.function_call.functions` 里加 `get_weather_now` 即可启用。

---

## 常见问题

<details>
<summary><b>刷完喊唤醒词没反应</b></summary>

先看屏幕：如果显示「没有连接网络」，是 WiFi 没连上（不是唤醒词问题）。

- 拔电重插，看是否恢复
- 检查路由器 2.4G 信号强度（RSSI 低于 -70 会不稳）
- 仍不行 → 按住 BOOT 上电进配网，重新填 WiFi
</details>

<details>
<summary><b>说话很慢 / 半天不出声</b></summary>

TTS 延迟问题。Edge TTS 在国内延迟 1~15 秒抖动严重，建议换成硅基流动等国内服务。

本版本默认用硅基流动 CosyVoice2，稳定在 1.5~2 秒。
</details>

<details>
<summary><b>定时提醒不响</b></summary>

- 设备必须开机且联网（时间来自服务端）
- 到点时如果正在对话，会在窗口内（5 分钟）延后播放
- 检查是否设置了「工作日」模式碰上周末
</details>

<details>
<summary><b>换音色后嘎嘎没变</b></summary>

`quack.ogg` 是编译期嵌进固件的，换音色后需要重新生成音频并重编：

```bash
# TTS 合成「嘎嘎！」→ mp3 → 转 OGG/Opus
ffmpeg -i quack.mp3 -af "silenceremove=start_periods=1:start_threshold=-45dB" \
  -ar 16000 -ac 1 -c:a libopus -b:a 24k quack.ogg
# 放到 main/assets/common/quack.ogg，然后重新编译
```
</details>

---

## 已知问题

- **没有 RTC**：时间完全依赖开机时从服务端同步，断网时提醒不可靠（加 DS3231 可解决）
- **提醒播完后麦克风仍开 2 分钟**（服务端 `close_connection_no_voice_time` 默认 120s）
- **唤醒词不可改**：`小鸭小鸭` 是编译期选定的 ESP-SR 模型，换词需要重编固件
- **角色音色版权**：爱弥斯 / 达妮娅是《鸣潮》（库洛游戏）的角色，零样本复刻的音色仅供个人使用，请勿商用

---

## 致谢与许可

本项目大量沿用以下开源工作：

- **[FlashCat-Jordan/Flashduck](https://github.com/FlashCat-Jordan/Flashduck)** —— 硬件设计、板级代码、步态算法（MIT）
- **[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)** —— 语音固件框架
- **[xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server)** —— 服务端
- **[XRailgunX/Aemeath-Audio-Dataset](https://huggingface.co/datasets/XRailgunX/Aemeath-Audio-Dataset)** —— 爱弥斯参考音频数据集（当前音色）
- **[huanx/daniya-voice-gptsovits](https://huggingface.co/datasets/huanx/daniya-voice-gptsovits)** —— 达妮娅参考音频数据集（上一版音色）
- **[Open-Meteo](https://open-meteo.com/)** —— 免 key 天气数据

固件代码沿用上游 **MIT License**；`firmware/duckclaw/` 目录（本项目未改动）为 **CC BY-NC-SA 4.0**。

---

<p align="center"><sub>一只住在桌上的小鲸，长着鸭子的身体 🐋🦆</sub></p>

---

## 附：自建 Web 控制面板

`panel/panel.py` —— 一个 30KB 的单文件 aiohttp 面板，替代官方控制台（官方那个功能太散）。

部署（与后端同机，挂在 8090）：

```yaml
services:
  panel:
    image: ghcr.nju.edu.cn/xinnan-tech/xiaozhi-esp32-server:server_latest
    container_name: duck-panel
    restart: always
    volumes:
      - ./panel:/app/panel:ro
      - ./data:/app/data
      - /var/run/docker.sock:/var/run/docker.sock
    working_dir: /app/panel
    command: python panel.py
    ports: ["127.0.0.1:8090:8090"]
```

访问 `http://<你的服务器>/admin/`，默认密码 `duck-2026-admin`（**请改掉**）。

能改的东西：人设、唤醒词、大模型、**长期记忆开关**、语音音色、技能启停、视觉模型、日志查看。
保存后会重建整份 `.config.yaml` 并通过 docker socket 重启后端。

---

## 附：音乐播放

设备端的 `NotifyPlayer` 只认 **OGG/Opus** 流，所以走「自建曲库 + 静态托管 + notify 推送」：

```
data/music/*.ogg   →  nginx  /music/  →  插件发 notify  →  设备拉流播放
```

**部署步骤**：

1. 曲库目录：`/home/ubuntu/xiaozhi-server/data/music/`
2. 网关挂载并开静态托管：

```yaml
# gateway/docker-compose.yml
volumes:
  - /home/ubuntu/xiaozhi-server/data/music:/usr/share/nginx/music:ro
```

```nginx
location /music/ {
    alias /usr/share/nginx/music/;
    add_header Cache-Control "public, max-age=3600";
}
```

3. 插件挂载（与天气插件同目录）：

```yaml
- ./plugins/music_play.py:/opt/xiaozhi-esp32-server/plugins_func/functions/music_play.py:ro
```

4. 面板「技能」里勾上 `self_music_play` / `self_music_list` / `self_music_stop`

**加歌**：用 `tools/convert-music.ps1` 把 mp3 批量转成 OGG，再传到 `data/music/`。

**三个工具**：

| 工具 | 触发话术 |
|---|---|
| `self_music_play` | 「放首歌」「我想听 XXX」「来点轻音乐」 |
| `self_music_list` | 「你能放什么歌」 |
| `self_music_stop` | 「别放了」「关掉音乐」 |

> 停止播放的原理：推一条 0.15 秒的静音 OGG，顶掉当前流（NotifyPlayer 只认 audio_url，没有独立的 stop 通道）。

---

## 附：屏幕截图接口

固件内置了一个调参用的截图接口，调眼睛/界面布局时不用靠猜：

```
GET http://<设备IP>/api/screen.bmp
```

返回当前屏幕的 240×240 24bit BMP（约 169 KB），用 LVGL 的 `lv_snapshot_take()` 实现。

取回并转成 PNG：

```powershell
powershell -ExecutionPolicy Bypass -File tools\screen-shot.ps1
powershell -ExecutionPolicy Bypass -File tools\screen-shot.ps1 -Device 192.168.1.11 -Out eye.png
```

> 注意：这是调试接口，抓屏时会临时分配 ~115 KB 内存。连续快速调用可能导致设备 HTTP 服务断连，间隔 2 秒以上比较稳。

### 控制台仪表盘

面板首页现在是一个实时仪表盘（每 5 秒刷新），改动配置的表单在下方：

| 卡片 | 内容 |
|---|---|
| **系统总览** | CPU / 内存 / 磁盘占用、已运行时长、当前大模型、语音音色、ASR、长期记忆开关、唤醒词、曲库数量 |
| **容器 / 进程** | `docker ps` 全量列表、运行状态、镜像，一键重启（白名单：后端 / 网关 / 面板） |
| **设备** | 局域网 IP 可编辑，探测在线状态，**实时显示鸭子屏幕**（走 `/api/screen.bmp`） |
| **服务端技能** | 当前启用的 function call 技能；折叠区列出固件端 33 个动作 / 10 种眼神 / 定时待办等能力清单 |
| **曲库** | 已入库曲目、大小、加入时间 |

> 「设备」卡片需要**浏览器和鸭子在同一 WiFi**：面板在云端，读不到你家里的局域网设备，
> 所以设备探测和截图都是在浏览器里直连设备 IP 完成的。
