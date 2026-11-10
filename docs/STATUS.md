# DuckRobort 项目进度

> 最后更新：2026-11-10 20:40
> 本文档记录当前实现状态、关键技术决策、踩过的坑和待办事项。
> **不含任何密码 / API Key / 设备 token**（仓库是公开的，敏感信息请查本地部署记录）。

---

## 一、一句话现状

机器鸭已经能**听、说、做动作、看东西、放歌、记事**，微信里也能和它聊天；
但**"定时拍照感知"和"远程指令"两个功能因为会让固件崩溃，目前处于关闭状态**。
摄像头硬件和视觉模型已升级到位（1024×768 + GLM-5.3-Flash），只差把常驻功能安全地接上。

---

## 二、硬件与网络

### 机器鸭

| 项 | 规格 |
|---|---|
| MCU | ESP32-S3，**16 MB Flash / 8 MB PSRAM** |
| 屏幕 | ST7789 240×240（双眼动画） |
| 摄像头 | **OV3660**，传感器最高 2048×1536，当前配置 **1024×768 (XGA)** |
| 舵机 | 6 路：左腿 5 / 右腿 43 / 左脚 6 / 右脚 44 / 脖子 7 / 嘴 4 |
| 音频 | I2S 麦克风 + 扬声器（**无 AEC 回采**） |
| 网络 | WiFi，局域网 IP `192.168.1.11` |

### 服务器

| 项 | 值 |
|---|---|
| 公网 IP | `124.220.41.99`（腾讯云） |
| 系统 | Ubuntu，部署目录 `/home/ubuntu/xiaozhi-server` |
| 面板地址 | `http://124.220.41.99/admin/` |
| OTA 地址 | `http://124.220.41.99/xiaozhi/ota/` |
| 面板端口 | 容器内 8090，经 nginx 反代到 `/admin/` |

---

## 三、软件架构

```
┌─────────────────────────────────────────────────────────┐
│ 机器鸭固件 (ESP32-S3)                                    │
│  · 语音助手：唤醒词「小鸭小鸭」→ WebSocket → 服务端        │
│  · 双眼 Kawaii 动画 + 唇形同步 + 33 个舵机动作           │
│  · 待办 / 提醒 / 计时 / 晨报（NVS 持久化）               │
│  · 摄像头拍照 → 上传服务端做视觉理解                     │
│  · 局域网调参页 /api/{status,walk,stop,gait,save,screen} │
└───────────────┬─────────────────────────────────────────┘
                │ WebSocket + HTTP
┌───────────────▼─────────────────────────────────────────┐
│ 服务端 xiaozhi-esp32-server (Docker)                     │
│  · LLM: DeepSeek deepseek-chat                          │
│  · ASR: SherpaASR（本地）  TTS: 硅基流动 CosyVoice2      │
│  · 视觉: GLM-5.3-Flash（reasoning_effort=low 补丁）      │
│  · 记忆: mem_local_short（data/.memory.yaml）            │
│  · 插件: 天气 / 音乐 / 联网搜索                          │
└───────────────┬─────────────────────────────────────────┘
                │
┌───────────────▼──────────────┐   ┌──────────────────────┐
│ 控制面板 duck-panel (:8090)   │   │ 微信桥 duck-clawbot   │
│  · 实时仪表盘 / 配置编辑      │◄──┤  · 长轮询收微信消息   │
│  · /api/xiaojing 小鲸大脑     │   │  · 调 /api/xiaojing   │
│  · /api/scene 画面记录        │   │  · 回复微信           │
│  · /api/cmd/* 指令队列        │   └──────────────────────┘
└──────────────────────────────┘
```

### 容器清单

| 容器 | 作用 | 网络 |
|---|---|---|
| `xiaozhi-esp32-server` | 语音后端 | site_default |
| `xiaozhi-gateway` | nginx，`/xiaozhi/*` `/admin/` `/music/` | 宿主 80 |
| `duck-panel` | 控制面板 | site_default |
| `duck-clawbot` | 微信 ClawBot 桥 | **site_default + xiaozhi-server_default** |
| `napcat` / `astrbot` | QQ 机器人（历史遗留，未使用） | — |

> ⚠️ `duck-clawbot` 必须同时接入 `site_default`，否则解析不到 `duck-panel`。

---

## 四、已完成的功能

### 固件端

| 功能 | 状态 | 说明 |
|---|---|---|
| 唤醒词「小鸭小鸭」 | ✅ | `wn9_xiaoyaxiaoya_tts2`，编译期固定 |
| 唤醒应答「我在呢」 | ✅ | 开麦前播放，避免自激 |
| 双眼 Kawaii 动画 | ✅ | 10 种神态、双高光、瞳孔呼吸、情绪配色 |
| 唇形同步 | ✅ | 按真实音频电平驱动嘴部舵机 |
| 33 个舵机动作 | ✅ | 走路花样 / 纯嘴 / 情绪 / 组合 |
| 待办清单 | ✅ | 语音记事 + 到点催办 + 问就报，NVS 持久化 |
| 定时提醒 / 倒计时 / 番茄钟 | ✅ | `self.schedule.*` `self.timer.*` |
| 每日晨报 | ✅ | `self.briefing.set` |
| 音乐播放 | ✅ | 服务端推送 OGG，`close_after_chat` 保证空闲时播放 |
| 联网看门狗 | ✅ | 50 秒无 IP 重启，2 次失败进配网 |
| OTA 自愈 | ✅ | 指数退避封顶 60 秒 |
| 摄像头拍照 | ✅ | OV3660，XGA 1024×768 |
| 局域网调参页 | ✅ | 步态调参 + 屏幕截图 `/api/screen.bmp` |
| 定时拍照感知 | ❌ **已关闭** | 会导致主任务卡死，见"踩坑记录" |
| 远程指令轮询 | ❌ **已关闭** | 板级初始化建任务导致启动卡死 |

### 服务端

| 功能 | 状态 |
|---|---|
| 人设「小鲸」 | ✅ 已去刻意化，自然口语风格 |
| 长期记忆 | ✅ `mem_local_short`，会记住聊过的事 |
| 天气（免 key） | ✅ Open-Meteo 插件 |
| 联网搜索 | ✅ Tavily |
| 视觉理解 | ✅ GLM-5.3-Flash + `reasoning_effort=low` |
| 静音判定 | ✅ 1000ms → **2000ms**（长句不再被切断） |
| 音乐曲库 | ✅ 2 首 OGG |

### 控制面板

| 功能 | 状态 |
|---|---|
| 实时仪表盘 | ✅ CPU / 内存 / 磁盘 / 容器 / 技能 / 曲库 |
| 配置编辑 | ✅ 人设 / 模型 / 语音 / 技能 / 视觉 |
| 容器重启 | ✅ 白名单一键重启 |
| 设备卡片 | ✅ 在线探测 + 实时屏幕截图（需浏览器同局域网） |
| 聊天测试 | ✅ |
| 日志查看 | ✅ |
| `/api/xiaojing` | ✅ 微信桥用的大脑接口（人设 + 记忆 + 指令识别） |
| `/api/scene` | ✅ 画面记录（暂无数据） |
| `/api/cmd/*` | ✅ 指令队列（设备暂不会取） |

### 微信

| 功能 | 状态 |
|---|---|
| ClawBot 绑定 | ✅ bot_id `4866a8***@im.bot`（完整值见本地记录） |
| 微信 ↔ 小鲸 双向聊天 | ✅ 实测可用 |
| 共享鸭子记忆 | ✅ 读同一份 `.memory.yaml` |
| 远程动作指令 | ⚠️ 能入队，但设备不会执行 |
| 收照片 | ❌ 未做 |

---

## 五、当前关键配置

```yaml
# data/.config.yaml 要点
selected_module:
  VAD: SileroVAD      # min_silence_duration_ms: 2000
  ASR: SherpaASR
  LLM: DeepSeekLLM    # deepseek-chat
  TTS: SiliconFlowTTS # speech:aemeath:...（爱弥斯）
  Memory: mem_local_short
  Intent: function_call
  VLLM: ChatGLMVLLM   # glm-5.3-flash
```

**启用的技能**：`web_search` `get_weather_now` `self_music_play` `self_music_list` `self_music_stop`

---

## 六、踩坑记录（重要，别再犯）

### 1. 板级初始化阶段创建 FreeRTOS 任务 → 启动卡死

```cpp
// flashduck_robot.cc 构造函数里
StartFlashduckRemoteCmdPoller();   // ← 加这行启动就停在 3.7 秒
```

现象：日志停在 `MCP 工具注册完成`，WiFi 不连接，状态机停在 `starting`。
去掉即恢复（1189ms 连上 WiFi）。

**结论：不要在板级初始化里 `xTaskCreate`。**

### 2. 把重活塞进 `sched_task` 的 1 秒 tick → 看门狗崩溃循环

定时拍照最初挂在 `ScheduleManager::Tick()` 里执行 XGA 拍照 + JPEG 编码。

根因：`Esp32Camera::Explain()` 内部 `std::thread` 用**默认优先级 5**，
而 `main` 任务优先级只有 **1**；XGA 编码要 1~2 秒，主任务全程抢不到 CPU，
`task_wdt` 报 `CPU 0: main` 卡住，屏幕停止刷新。

**结论：**
- 摄像头编码不能和 UI/音频争 CPU；
- 拍照前把传感器降到 VGA（`sensor->set_framesize()`）；
- 或者用独立的低优先级任务。

### 3. RGB565 模式下分辨率受 PSRAM 限制

| 分辨率 | 单帧 | 两帧缓冲 + 编码缓冲 |
|---|---|---|
| 240×240 | 115 KB | ~350 KB |
| 640×480 | 614 KB | ~1.8 MB |
| **1024×768** | **1.5 MB** | **~4.7 MB** ✓ |
| 1280×960 | 2.4 MB | ~7.4 MB ✗ 太挤 |
| 2048×1536 | 6.3 MB | 18.9 MB ✗ 不可能 |

想用满 3MP 必须改成 **PIXFORMAT_JPEG**（传感器直出），但目前 `Explain()` 的
编码线程只处理 RGB565/YUV，需要改造。

### 4. PowerShell 读写 UTF-8 文件会毁掉中文

`Get-Content -Raw | Set-Content` 会把 UTF-8 当 GBK 读再写回，
导致源码编译失败、README 在 GitHub 上整段乱码。

**结论：改文件只用编辑工具，不要用 PowerShell 往返读写。**

### 5. 唤醒词参数容易打错

正确：`wn9_xiaoyaxiaoya_tts2`
错误：`wn9_xiaoxiaoyaoya_tts2`（多一个 o）
打错时 build.py 会报 `Kconfig rejected`，不会静默成功。

### 6. 音频文件不是"能播就行"，要看响度

`quack.ogg` 平均响度只有 −32.5 dB，叠加音量缩放后等于没声音。
修复：压缩器 + EBU R128 标准化到 −22 dB / 峰值 −1.8 dB。

---

## 七、未完成事项

### 高优先级

| 项 | 说明 | 风险 |
|---|---|---|
| **定时拍照感知** | 代码已写好（`flashduck_scene.cc`），需改成独立低优先级任务 + VGA 降采样，并逐项验证 | 中，曾导致崩溃 |
| **远程指令执行** | 服务端队列已通，设备端轮询代码存在但被禁用 | 中，曾导致启动卡死 |
| **`look_for` 找东西** | 转头扫三个角度拍照分析，代码已写未验证 | 中 |

### 中优先级

| 项 | 说明 |
|---|---|
| 微信收照片 | 需要确认 ClawBot 图片消息协议；退路是发链接 |
| OCR / 翻译 | 拍书、快递单、菜单 |
| 认人 + 记忆 | 靠 VLM 文字描述近似匹配，不做人脸库 |
| 面板「最近看到」卡片 | `/api/scene` 已有数据接口，前端未展示 |

### 低优先级

| 项 | 说明 |
|---|---|
| 清理 vikunja 容器 | 已停止 22 小时以上 |
| 清理 napcat / astrbot | QQ 方案未采用 |
| 白噪音助眠 | 需加音频素材 |

---

## 八、运维手册

### 重新编译固件

```bash
cd firmware/voice
. D:\esp\esp-idf\export.ps1
python scripts/build.py flashduck --name flashduck --language zh-CN \
  --wake-word wn9_xiaoyaxiaoya_tts2
```

### 烧录

```bash
# 仅应用（保留 WiFi / 步态 / 待办）
esptool --chip esp32s3 -p COM3 -b 460800 write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x20000 build/xiaozhi.bin
```

### 日常检查

```bash
# 设备在线
curl http://192.168.1.11/api/status

# 设备屏幕截图
powershell -ExecutionPolicy Bypass -File tools/screen-shot.ps1 -Out eye.png

# 面板仪表盘
curl http://124.220.41.99/admin/api/overview

# 服务端健康
docker logs --tail 50 xiaozhi-esp32-server
```

### 面板接口一览

| 接口 | 用途 |
|---|---|
| `GET /admin/api/overview` | 主机 + 容器 + 配置 + 技能 + 曲库 |
| `POST /admin/api/restart/{name}` | 重启容器（白名单） |
| `POST /admin/api/xiaojing` | 小鲸对话（微信桥调用） |
| `POST /admin/api/scene` | 设备上传画面描述 |
| `GET /admin/api/scene` | 读取最近画面描述 |
| `GET /admin/api/cmd/pending` | 设备取远程指令 |
| `POST /admin/api/cmd/ack` | 设备回执 |
| `GET /admin/api/cmd/list` | 面板查看指令队列 |

### 设备接口一览

| 接口 | 用途 |
|---|---|
| `GET /api/status` | `{"moving":false}` |
| `GET /api/screen.bmp` | 240×240 屏幕截图 |
| `POST /api/walk` `POST /api/stop` | 步态控制 |
| `POST /api/gait` `POST /api/save` | 步态调参 |

---

## 九、版本记录

| 提交 | 内容 |
|---|---|
| `2a81411` | 回滚定时拍照（主任务卡死） |
| `c360766` | 摄像头 240×240 → VGA |
| `d84f594` | 静音判定 1s→2s、人设去刻意化 |
| `5239957` | 关闭远程指令轮询（启动卡死） |
| `20ac57c` | 接入微信 ClawBot + 指令队列 |
| `3a0b949` | 嘎嘎音效响度标准化 |
| `cc65892` | README 音色表述修正（达妮娅→爱弥斯） |
| `4027d19` | README 固件下载入口 + 版本徽章 |
| `0a22310` | 屏幕截图工具 + README 说明 |
| `aed4da7` | 双眼虹膜偏移修复（清 padding） |
| `c019539` | 单眼改双眼 Kawaii |

**固件 Release**：`fw-v1.0.0`（双眼 Kawaii 版）

---

## 十、当前工作区状态

⚠️ **有未提交的改动**（场景功能第二版，**未编译、未刷机**）：

```
M firmware/voice/main/boards/flashduck/flashduck_controller.cc
M firmware/voice/main/boards/flashduck/flashduck_scene.cc
M firmware/voice/main/boards/flashduck/flashduck_scene.h
M firmware/voice/main/boards/flashduck/schedule_manager.cc
```

设备上跑的是 `2a81411` 的构建（XGA 摄像头，无场景功能，无远程指令）。

**恢复干净状态**：

```bash
cd D:\project\Flashduck
git checkout -- firmware/voice/main/boards/flashduck/
```

**或者继续开发**：这些改动是"独立低优先级任务 + VGA 降采样"的第二版方案，
编译后再刷机验证即可。
