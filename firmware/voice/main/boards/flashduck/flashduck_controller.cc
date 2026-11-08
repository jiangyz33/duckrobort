#include "flashduck_controller.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "application.h"
#include "assets/lang_config.h"
#include "audio/lip_sync.h"
#include "board.h"
#include "flashduck_movements.h"
#include "mcp_server.h"
#include "power_manager.h"
#include "schedule_manager.h"
#include "settings.h"
#include "wifi_manager.h"

#define TAG "FlashduckController"

// 动作队列深度：满了就如实报错，避免模型以为动作执行了
static constexpr int kActionQueueLen = 10;

class FlashduckController {
private:
    Flashduck duck_;
    TaskHandle_t action_task_handle_ = nullptr;
    QueueHandle_t action_queue_ = nullptr;
    bool has_head_ = false;
    bool is_action_in_progress_ = false;
    uint32_t next_idle_ms_ = 0;
    // 说话嘴型（由真实音频电平驱动）
    int mouth_pos_ = -1;
    uint32_t next_mouth_ms_ = 0;
    FlashduckGaitParams gait_defaults_{FLASHDUCK_WALK_HIP_OFFSET, FLASHDUCK_WALK_HIP_AMP,
                                       FLASHDUCK_WALK_FOOT_AMP, FLASHDUCK_WALK_PHASE_DEG,
                                       FLASHDUCK_WALK_SPEED, FLASHDUCK_WALK_HEAD_SWING};

    struct ActionParams {
        int action_type;
        int steps;
        int speed;
        int direction;
        int amount;
        int head_swing;
        int hip_offset;
        int hip_amp;
        int foot_amp;
        int phase_deg;
    };

    enum ActionType {
        ACTION_WALK = 1,
        ACTION_TURN = 2,
        ACTION_JUMP = 3,
        ACTION_SWING = 4,
        ACTION_MOONWALK = 5,
        ACTION_BEND = 6,
        ACTION_SHAKE_LEG = 7,
        ACTION_UPDOWN = 8,
        ACTION_WHIRLWIND_LEG = 9,
        ACTION_SIT = 10,
        ACTION_SHOWCASE = 11,
        ACTION_HOME = 12,
        ACTION_HEAD_TURN = 20,
        ACTION_HEAD_SHAKE = 21,
        ACTION_LOOK_AROUND = 22,
        ACTION_MOUTH_OPEN = 23,
        ACTION_MOUTH_CLOSE = 24,
        ACTION_MOUTH_MOVE = 25,
        ACTION_TALK = 26,
        ACTION_LAUGH = 27,
        ACTION_SING = 28,
        ACTION_YAWN = 29,
        ACTION_THINK = 30,
        ACTION_HAPPY = 31,
        ACTION_SLEEPY = 32,
        // 鸭子专属动作
        ACTION_QUACK = 40,
        ACTION_CHATTER = 41,
        ACTION_CURIOUS = 42,
        ACTION_PREEN = 43,
        ACTION_HAPPY_WADDLE = 44,
        ACTION_STRETCH = 45,
        ACTION_DANCE = 46,
        ACTION_PECK = 47,
        ACTION_STOP = 99,
    };

    static void ActionTask(void* arg) {
        auto* self = static_cast<FlashduckController*>(arg);
        ActionParams params;
        self->duck_.AttachServos();

        while (true) {
            // 说话时把轮询提速到 60ms，才能做出有节奏的嘴型
            const bool speaking =
                Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;
            const TickType_t wait = speaking ? pdMS_TO_TICKS(60) : pdMS_TO_TICKS(200);
            if (xQueueReceive(self->action_queue_, &params, wait) != pdTRUE) {
                self->MaybeIdleGesture();
                self->MaybeSpeakingMouth();
                continue;
            }

            ESP_LOGI(TAG, "执行动作: %d", params.action_type);
            PowerManager::PauseBatteryUpdate();
            self->is_action_in_progress_ = true;
            self->duck_.ClearStopRequest();

            switch (params.action_type) {
                case ACTION_WALK:
                    self->duck_.Walk(params.steps, params.speed, params.direction, params.head_swing,
                                     params.hip_offset, params.hip_amp, params.foot_amp,
                                     params.phase_deg);
                    break;
                case ACTION_TURN:
                    self->duck_.Turn(params.steps, params.speed, params.direction, params.head_swing,
                                     params.hip_offset, params.hip_amp, params.foot_amp,
                                     params.phase_deg);
                    break;
                case ACTION_JUMP:
                    self->duck_.Jump(params.steps, params.speed);
                    break;
                case ACTION_SWING:
                    self->duck_.Swing(params.steps, params.speed, params.amount);
                    break;
                case ACTION_MOONWALK:
                    self->duck_.Moonwalker(params.steps, params.speed, params.amount,
                                           params.direction);
                    break;
                case ACTION_BEND:
                    self->duck_.Bend(params.steps, params.speed, params.direction);
                    break;
                case ACTION_SHAKE_LEG:
                    self->duck_.ShakeLeg(params.steps, params.speed, params.direction);
                    break;
                case ACTION_UPDOWN:
                    self->duck_.UpDown(params.steps, params.speed, params.amount);
                    break;
                case ACTION_WHIRLWIND_LEG:
                    self->duck_.WhirlwindLeg(params.steps, params.speed, params.amount);
                    break;
                case ACTION_SIT:
                    self->duck_.Sit();
                    break;
                case ACTION_SHOWCASE:
                    self->duck_.Showcase();
                    break;
                case ACTION_HOME:
                    self->duck_.Home(true);
                    break;
                case ACTION_HEAD_TURN:
                    if (self->has_head_) {
                        self->duck_.HeadTurn(params.direction, params.speed);
                    }
                    break;
                case ACTION_HEAD_SHAKE:
                    if (self->has_head_) {
                        self->duck_.HeadShake(params.steps, params.speed, params.amount);
                    }
                    break;
                case ACTION_LOOK_AROUND:
                    if (self->has_head_) {
                        self->duck_.HeadLookAround(params.speed);
                    }
                    break;
                case ACTION_MOUTH_OPEN:
                    if (self->has_head_) {
                        self->duck_.MouthOpen(params.speed);
                    }
                    break;
                case ACTION_MOUTH_CLOSE:
                    if (self->has_head_) {
                        self->duck_.MouthClose(params.speed);
                    }
                    break;
                case ACTION_MOUTH_MOVE:
                    if (self->has_head_) {
                        self->duck_.MouthMove(params.steps, params.speed, params.amount);
                    }
                    break;
                case ACTION_TALK:
                    if (self->has_head_) {
                        self->duck_.Talk(params.steps, params.speed, params.amount);
                    }
                    break;
                case ACTION_LAUGH:
                    if (self->has_head_) {
                        self->duck_.Laugh(params.steps, params.speed, params.amount);
                    }
                    break;
                case ACTION_SING:
                    if (self->has_head_) {
                        self->duck_.Sing(params.steps, params.speed, params.amount);
                    }
                    break;
                case ACTION_YAWN:
                    if (self->has_head_) {
                        self->duck_.Yawn(params.speed);
                    }
                    break;
                case ACTION_THINK:
                    if (self->has_head_) {
                        self->duck_.Think(params.steps, params.speed, params.amount);
                    }
                    break;
                case ACTION_HAPPY:
                    if (self->has_head_) {
                        self->duck_.Happy(params.steps, params.speed, params.amount);
                    }
                    break;
                case ACTION_SLEEPY:
                    if (self->has_head_) {
                        self->duck_.Sleepy(params.steps, params.speed, params.amount);
                    }
                    break;
                case ACTION_QUACK: {
                    if (!self->has_head_) break;
                    const int cycles = std::clamp(params.steps, 1, 3);
                    const int amplitude = std::clamp(params.amount, 5, 12);
                    const int period = std::clamp(params.speed, 250, 600);
                    // 先出声再动嘴，用的是 assets/common/quack.ogg（达妮娅音色合成）
                    Application::GetInstance().PlaySound(Lang::Sounds::OGG_QUACK);
                    self->duck_.IdleLook(0, cycles, amplitude, 300, period, period);
                    break;
                }
                case ACTION_CHATTER: {
                    if (!self->has_head_) break;
                    const int cycles = std::clamp(params.steps + 2, 3, 7);
                    const int amplitude = std::clamp(params.amount, 3, 6);
                    const int period = std::clamp(params.speed, 160, 320);
                    self->duck_.IdleLook(0, cycles, amplitude, 200, period, period);
                    break;
                }
                case ACTION_CURIOUS: {
                    if (!self->has_head_) break;
                    const int delta = params.direction >= 0 ? -12 : 12;
                    const int period = std::clamp(params.speed, 350, 900);
                    self->duck_.IdleLook(delta, 2, 8, period, period / 2, period / 2);
                    break;
                }
                case ACTION_PREEN: {
                    if (!self->has_head_) break;
                    const int delta = params.direction >= 0 ? -15 : 15;
                    const int period = std::clamp(params.speed, 450, 1000);
                    self->duck_.IdleLook(delta, 3, 5, period, period / 2, period / 2);
                    break;
                }
                case ACTION_HAPPY_WADDLE: {
                    const auto& g = self->gait_defaults_;
                    self->duck_.Walk(2, g.speed, params.direction, g.head_swing, g.hip_offset,
                                     g.hip_amp, g.foot_amp, g.phase_deg);
                    if (self->has_head_) {
                        self->duck_.IdleLook(0, 2, 9, 300, 240, 240);
                    }
                    break;
                }
                case ACTION_STRETCH: {
                    const int period = std::clamp(params.speed, 650, 1200);
                    self->duck_.UpDown(1, period, 15);
                    if (self->has_head_) {
                        self->duck_.Yawn(period);
                    }
                    break;
                }
                case ACTION_DANCE: {
                    const auto& g = self->gait_defaults_;
                    const int period = std::clamp(params.speed, 500, 1000);
                    self->duck_.Walk(2, period, FORWARD, 25, g.hip_offset, g.hip_amp, g.foot_amp,
                                     g.phase_deg);
                    if (self->has_head_) {
                        self->duck_.IdleLook(-14, 2, 8, period / 2, period / 3, period / 3);
                    }
                    self->duck_.Walk(2, period, BACKWARD, 25, g.hip_offset, g.hip_amp, g.foot_amp,
                                     g.phase_deg);
                    if (self->has_head_) {
                        self->duck_.IdleLook(14, 2, 8, period / 2, period / 3, period / 3);
                        self->duck_.IdleLook(0, 3, 10, period / 2, period / 3, period / 3);
                    }
                    break;
                }
                case ACTION_PECK: {
                    const int period = std::clamp(params.speed, 450, 900);
                    self->duck_.UpDown(1, period, 18);
                    if (self->has_head_) {
                        self->duck_.IdleLook(0, 1, 12, 250, 200, 200);
                    }
                    break;
                }
                case ACTION_STOP:
                    self->duck_.RequestStop();
                    self->duck_.Home(true);
                    break;
                default:
                    break;
            }

            if (params.action_type != ACTION_SIT && params.action_type != ACTION_HOME &&
                params.action_type != ACTION_STOP) {
                // 转头/张嘴/闭嘴是“保持姿态”的动作，只复位腿脚；
                // 其余动作结束后完整回到中立姿态。
                const bool head_reset = !(params.action_type == ACTION_HEAD_TURN ||
                                          params.action_type == ACTION_MOUTH_OPEN ||
                                          params.action_type == ACTION_MOUTH_CLOSE);
                self->duck_.Home(head_reset);
            }

            self->is_action_in_progress_ = false;
            PowerManager::ResumeBatteryUpdate();
            self->next_idle_ms_ = NowMs() + 8000 + (esp_random() % 8000);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    static uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

    // 说话嘴型：由音频输出链路的真实响度驱动（见 audio/lip_sync.h）。
    // 不再按设备状态猜——缓冲/静音阶段电平为 0 或过期，嘴巴保持闭合，
    // 所以不会出现"先张嘴、TTS 后出声"的错位。
    void MaybeSpeakingMouth() {
        if (!has_head_ || is_action_in_progress_) {
            return;
        }

        const uint32_t now = NowMs();
        const bool playing = LipSync::Fresh(now, 250);
        const int level = playing ? static_cast<int>(LipSync::Level()) : 0;

        if (!playing || level < 5) {
            if (mouth_pos_ != MOUTH_CLOSED) {
                duck_.MoveSingle(MOUTH_CLOSED, MOUTH);
                mouth_pos_ = MOUTH_CLOSED;
            }
            return;
        }

        if (mouth_pos_ != MOUTH_CLOSED &&
            static_cast<int32_t>(now - next_mouth_ms_) < 0) {
            return;
        }

        // 响度映射到开口 2~12°；嘴巴硬限位 45~60°，留足余量
        const int open = 2 + level * 10 / 100;
        const int jitter = static_cast<int>(esp_random() % 3) - 1;
        int pos = MOUTH_CLOSED + open + jitter;
        if (pos < MOUTH_CLOSED) {
            pos = MOUTH_CLOSED;
        }
        if (pos > 57) {
            pos = 57;
        }

        duck_.MoveSingle(pos, MOUTH);
        mouth_pos_ = pos;
        next_mouth_ms_ = now + 45;  // 约 22Hz，够跟上语速
    }

    void MaybeIdleGesture() {
        if (!has_head_ || is_action_in_progress_) {
            return;
        }

        const uint32_t now = NowMs();
        if (next_idle_ms_ == 0) {
            next_idle_ms_ = now + 8000 + (esp_random() % 8000);
            return;
        }
        if (static_cast<int32_t>(now - next_idle_ms_) < 0) {
            return;
        }

        // 只在本体完全空闲/待机时做；正在听、说、连网时都不插动作。
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            next_idle_ms_ = now + 5000;
            return;
        }

        const int dir = (esp_random() % 2) == 0 ? -1 : 1;
        const int neck_delta = (8 + (esp_random() % 11)) * dir;  // 轻微 ±8~18°
        const int mouth_cycles = 2 + (esp_random() % 3);          // 张合 2~4 次
        const int mouth_amplitude = 5 + (esp_random() % 8);       // 每次只开 5~12°
        const int move_ms = 350 + (esp_random() % 350);
        const int open_ms = 180 + (esp_random() % 220);
        const int close_ms = 180 + (esp_random() % 220);

        ESP_LOGI(TAG, "空闲小动作: neck=%+d° mouth=%d×%d° move=%d open=%d close=%d",
                 neck_delta, mouth_cycles, mouth_amplitude, move_ms, open_ms, close_ms);
        duck_.IdleLook(neck_delta, mouth_cycles, mouth_amplitude, move_ms, open_ms, close_ms);

        next_idle_ms_ = NowMs() + 10000 + (esp_random() % 12000);
    }

    void StartActionTaskIfNeeded() {
        if (action_task_handle_ == nullptr) {
            xTaskCreate(ActionTask, "flashduck_action", 4096, this, 5, &action_task_handle_);
        }
    }

    bool QueueAction(int action_type, int steps, int speed, int direction, int amount,
                     int head_swing = 0,
                     int hip_offset = FLASHDUCK_WALK_HIP_OFFSET,
                     int hip_amp = FLASHDUCK_WALK_HIP_AMP,
                     int foot_amp = FLASHDUCK_WALK_FOOT_AMP,
                     int phase_deg = FLASHDUCK_WALK_PHASE_DEG) {
        ActionParams params = {action_type, steps, speed, direction, amount, head_swing,
                               hip_offset, hip_amp, foot_amp, phase_deg};
        // 队列满了要如实告诉调用方，否则模型会以为动作已经执行了
        if (xQueueSend(action_queue_, &params, 0) != pdTRUE) {
            ESP_LOGW(TAG, "动作队列已满(%d)，丢弃 action=%d", kActionQueueLen, action_type);
            return false;
        }
        StartActionTaskIfNeeded();
        return true;
    }

    FlashduckGaitParams ClampGait(FlashduckGaitParams gait) const {
        gait.hip_offset = std::clamp(gait.hip_offset, 0, 25);
        gait.hip_amp = std::clamp(gait.hip_amp, 15, 50);
        gait.foot_amp = std::clamp(gait.foot_amp, 15, 50);
        gait.phase_deg = std::clamp(gait.phase_deg, 60, 120);
        gait.speed = std::clamp(gait.speed, 400, 2000);
        gait.head_swing = std::clamp(gait.head_swing, 0, 90);
        return gait;
    }

    void LoadGaitDefaults() {
        bool missing = true;
        {
            Settings settings("flashduck_gait", false);
            int speed = settings.GetInt("speed", -1);
            if (speed >= 0) {
                missing = false;
                gait_defaults_.hip_offset =
                    settings.GetInt("hip_offset", gait_defaults_.hip_offset);
                gait_defaults_.hip_amp = settings.GetInt("hip_amp", gait_defaults_.hip_amp);
                gait_defaults_.foot_amp =
                    settings.GetInt("foot_amp", gait_defaults_.foot_amp);
                gait_defaults_.phase_deg =
                    settings.GetInt("phase_deg", gait_defaults_.phase_deg);
                gait_defaults_.speed = speed;
                gait_defaults_.head_swing =
                    settings.GetInt("head_swing", gait_defaults_.head_swing);
            }
        }
        gait_defaults_ = ClampGait(gait_defaults_);
        if (missing) {
            // 第一次启动：把编译期推荐值写进 NVS，保证语音和网页默认一致。
            SaveGaitDefaults(gait_defaults_);
            ESP_LOGI(TAG, "首次写入默认步态到 NVS");
        }
        ESP_LOGI(TAG, "步态默认值: offset=%d amp=%d foot=%d phase=%d speed=%d head=%d",
                 gait_defaults_.hip_offset, gait_defaults_.hip_amp, gait_defaults_.foot_amp,
                 gait_defaults_.phase_deg, gait_defaults_.speed, gait_defaults_.head_swing);
    }

    void LoadTrimsFromNVS() {
        Settings settings("flashduck_trims", false);
        int left_leg = settings.GetInt("left_leg", 0);
        int right_leg = settings.GetInt("right_leg", 0);
        int left_foot = settings.GetInt("left_foot", 0);
        int right_foot = settings.GetInt("right_foot", 0);
        int neck = settings.GetInt("neck", 0);
        int mouth = settings.GetInt("mouth", 0);

        ESP_LOGI(TAG, "从NVS加载微调: LL=%d RL=%d LF=%d RF=%d NK=%d MW=%d", left_leg, right_leg,
                 left_foot, right_foot, neck, mouth);
        duck_.SetTrims(left_leg, right_leg, left_foot, right_foot, neck, mouth);
    }

public:
    explicit FlashduckController(const HardwareConfig& hw_config) {
        // 摄像头版的 left_hand_pin/right_hand_pin 实际接嘴巴/脖子：
        // 嘴巴 = 原左手位 GPIO4，脖子 = 原右手位 GPIO7。
        duck_.Init(hw_config.left_leg_pin, hw_config.right_leg_pin, hw_config.left_foot_pin,
                   hw_config.right_foot_pin, hw_config.right_hand_pin, hw_config.left_hand_pin);

        has_head_ = (hw_config.left_hand_pin != GPIO_NUM_NC &&
                     hw_config.right_hand_pin != GPIO_NUM_NC);
        ESP_LOGI(TAG, "Flashduck 初始化%s头部舵机（脖子/嘴巴）", has_head_ ? "带" : "不带");
        ESP_LOGI(TAG, "舵机引脚: LL=%d RL=%d LF=%d RF=%d NK=%d MW=%d", hw_config.left_leg_pin,
                 hw_config.right_leg_pin, hw_config.left_foot_pin, hw_config.right_foot_pin,
                 hw_config.right_hand_pin, hw_config.left_hand_pin);

        LoadGaitDefaults();
        LoadTrimsFromNVS();
        action_queue_ = xQueueCreate(kActionQueueLen, sizeof(ActionParams));
        QueueAction(ACTION_HOME, 1, 1000, 0, 0);
        RegisterMcpTools();
    }

    ~FlashduckController() {
        if (action_task_handle_ != nullptr) {
            vTaskDelete(action_task_handle_);
            action_task_handle_ = nullptr;
        }
        if (action_queue_ != nullptr) {
            vQueueDelete(action_queue_);
            action_queue_ = nullptr;
        }
    }

    void StartWalk(int steps, int speed, int direction, int hip_offset, int hip_amp,
                   int foot_amp, int phase_deg, int head_swing) {
        steps = std::clamp(steps, 1, 10);
        speed = std::clamp(speed, 400, 2000);
        direction = direction >= 0 ? 1 : -1;
        hip_offset = std::clamp(hip_offset, 0, 25);
        hip_amp = std::clamp(hip_amp, 15, 50);
        foot_amp = std::clamp(foot_amp, 15, 50);
        phase_deg = std::clamp(phase_deg, 60, 120);
        head_swing = std::clamp(head_swing, 0, 90);
        QueueAction(ACTION_WALK, steps, speed, direction, 0, head_swing, hip_offset, hip_amp,
                    foot_amp, phase_deg);
    }

    void Stop() {
        duck_.RequestStop();
        QueueAction(ACTION_STOP, 1, 1000, 0, 0);
    }

    FlashduckGaitParams GetGaitDefaults() const { return gait_defaults_; }

    void SaveGaitDefaults(const FlashduckGaitParams& gait) {
        gait_defaults_ = ClampGait(gait);
        Settings settings("flashduck_gait", true);
        settings.SetInt("hip_offset", gait_defaults_.hip_offset);
        settings.SetInt("hip_amp", gait_defaults_.hip_amp);
        settings.SetInt("foot_amp", gait_defaults_.foot_amp);
        settings.SetInt("phase_deg", gait_defaults_.phase_deg);
        settings.SetInt("speed", gait_defaults_.speed);
        settings.SetInt("head_swing", gait_defaults_.head_swing);
        ESP_LOGI(TAG, "步态默认值已保存: offset=%d amp=%d foot=%d phase=%d speed=%d head=%d",
                 gait_defaults_.hip_offset, gait_defaults_.hip_amp, gait_defaults_.foot_amp,
                 gait_defaults_.phase_deg, gait_defaults_.speed, gait_defaults_.head_swing);
    }

    bool IsMoving() const { return is_action_in_progress_; }

    void RegisterMcpTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool(
            "self.flashduck.action",
            "执行 Flashduck 机器鸭动作。walk/turn 使用机器人 NVS 保存的默认步态。"
            "基础：walk(行走)、turn(转身)、jump(跳一下)、swing(左右摇摆)、updown(上下蹲起)、"
            "sit(坐下)、home(复位)；"
            "脖子：head_turn(转头，direction=1左/-1右/0回正)、head_shake(摇头)、look_around(环顾)；"
            "嘴巴和趣味动作：quack(张嘴合嘴1~3次，steps=次数，amount=开口5~12)、"
            "chatter(快速小声咂嘴)、curious(侧头好奇+轻叫)、preen(侧头理羽+咂嘴)、"
            "happy_waddle(开心走两步+轻叫)、stretch(伸懒腰)、dance(小段鸭舞)、"
            "peck(低头啄食+张嘴)、yawn(打哈欠)、sleepy(困倦)；"
            "走路花样：moonwalk(太空步)、bend(弯腰)、shake_leg(抖腿)、whirlwind_leg(风车腿)、"
            "showcase(动作串烧展示)；纯嘴与情绪：mouth_open(张嘴保持)、mouth_close(闭嘴保持)、"
            "mouth_move(嘴巴开合)、talk(说话嘴型)、laugh(大笑)、sing(唱歌嘴型)、"
            "think(思考：侧头+小幅嘴动)、happy(开心：小摇头+嘴开合)。"
            "direction=1 表示前进/左转/左看，-1 表示后退/右转/右看，0 表示回正。",
            PropertyList({
                Property("action", kPropertyTypeString, "home"),
                Property("steps", kPropertyTypeInteger, 3, 1, 100),
                Property("speed", kPropertyTypeInteger, 700, 100, 3000),
                Property("direction", kPropertyTypeInteger, 1, -1, 1),
                Property("amount", kPropertyTypeInteger, 30, 0, 170),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();
                int steps = properties["steps"].value<int>();
                int speed = properties["speed"].value<int>();
                int direction = properties["direction"].value<int>();
                int amount = properties["amount"].value<int>();

                // 队列满了就直接报错，别让模型以为动作已经执行了
                if (action != "stop" && uxQueueSpacesAvailable(action_queue_) == 0) {
                    return "错误：动作队列已满（机器人正在执行别的动作），"
                           "请稍后再试；要打断它就先调用 self.flashduck.stop";
                }

                if (action == "walk") {
                    const auto& g = gait_defaults_;
                    QueueAction(ACTION_WALK, steps, g.speed, direction, 0, g.head_swing,
                                g.hip_offset, g.hip_amp, g.foot_amp, g.phase_deg);
                } else if (action == "turn") {
                    const auto& g = gait_defaults_;
                    QueueAction(ACTION_TURN, steps, g.speed, direction, 0, g.head_swing,
                                g.hip_offset, g.hip_amp, g.foot_amp, g.phase_deg);
                } else if (action == "jump") {
                    QueueAction(ACTION_JUMP, steps, speed, 0, 0);
                } else if (action == "swing") {
                    QueueAction(ACTION_SWING, steps, speed, 0, amount);
                } else if (action == "updown") {
                    QueueAction(ACTION_UPDOWN, steps, speed, 0, amount);
                } else if (action == "sit") {
                    QueueAction(ACTION_SIT, 1, 0, 0, 0);
                } else if (action == "home") {
                    QueueAction(ACTION_HOME, 1, speed, 0, 0);
                } else if (action == "head_turn") {
                    if (!has_head_) return "错误：没有脖子舵机";
                    QueueAction(ACTION_HEAD_TURN, 1, speed, direction, 0);
                } else if (action == "head_shake") {
                    if (!has_head_) return "错误：没有脖子舵机";
                    QueueAction(ACTION_HEAD_SHAKE, steps, speed, 0, amount);
                } else if (action == "look_around") {
                    if (!has_head_) return "错误：没有脖子舵机";
                    QueueAction(ACTION_LOOK_AROUND, 1, speed, 0, 0);
                } else if (action == "quack") {
                    if (!has_head_) return "错误：没有嘴巴舵机";
                    QueueAction(ACTION_QUACK, steps, speed, 0, amount);
                } else if (action == "chatter") {
                    if (!has_head_) return "错误：没有嘴巴舵机";
                    QueueAction(ACTION_CHATTER, steps, speed, 0, amount);
                } else if (action == "curious") {
                    if (!has_head_) return "错误：没有脖子/嘴巴舵机";
                    QueueAction(ACTION_CURIOUS, 1, speed, direction, amount);
                } else if (action == "preen") {
                    if (!has_head_) return "错误：没有脖子/嘴巴舵机";
                    QueueAction(ACTION_PREEN, 1, speed, direction, amount);
                } else if (action == "happy_waddle") {
                    QueueAction(ACTION_HAPPY_WADDLE, 2, speed, direction, 0);
                } else if (action == "stretch") {
                    QueueAction(ACTION_STRETCH, 1, speed, 0, 0);
                } else if (action == "dance") {
                    QueueAction(ACTION_DANCE, 1, speed, direction, amount);
                } else if (action == "peck") {
                    QueueAction(ACTION_PECK, 1, speed, direction, amount);
                } else if (action == "yawn") {
                    if (!has_head_) return "错误：没有嘴巴舵机";
                    QueueAction(ACTION_YAWN, 1, speed, 0, 0);
                } else if (action == "sleepy") {
                    if (!has_head_) return "错误：没有嘴巴/脖子舵机";
                    QueueAction(ACTION_SLEEPY, steps, speed, 0, amount);
                } else if (action == "moonwalk") {
                    QueueAction(ACTION_MOONWALK, steps, speed, direction, amount);
                } else if (action == "bend") {
                    QueueAction(ACTION_BEND, steps, speed, direction, 0);
                } else if (action == "shake_leg") {
                    QueueAction(ACTION_SHAKE_LEG, steps, speed, direction, 0);
                } else if (action == "whirlwind_leg") {
                    QueueAction(ACTION_WHIRLWIND_LEG, steps, speed, 0, amount);
                } else if (action == "showcase") {
                    QueueAction(ACTION_SHOWCASE, 1, speed, 0, 0);
                } else if (action == "mouth_open") {
                    if (!has_head_) return "错误：没有嘴巴舵机";
                    QueueAction(ACTION_MOUTH_OPEN, 1, speed, 0, 0);
                } else if (action == "mouth_close") {
                    if (!has_head_) return "错误：没有嘴巴舵机";
                    QueueAction(ACTION_MOUTH_CLOSE, 1, speed, 0, 0);
                } else if (action == "mouth_move") {
                    if (!has_head_) return "错误：没有嘴巴舵机";
                    QueueAction(ACTION_MOUTH_MOVE, steps, speed, 0, amount);
                } else if (action == "talk") {
                    if (!has_head_) return "错误：没有嘴巴舵机";
                    QueueAction(ACTION_TALK, steps, speed, 0, amount);
                } else if (action == "laugh") {
                    if (!has_head_) return "错误：没有嘴巴舵机";
                    QueueAction(ACTION_LAUGH, steps, speed, 0, amount);
                } else if (action == "sing") {
                    if (!has_head_) return "错误：没有嘴巴舵机";
                    QueueAction(ACTION_SING, steps, speed, 0, amount);
                } else if (action == "think") {
                    if (!has_head_) return "错误：没有嘴巴/脖子舵机";
                    QueueAction(ACTION_THINK, steps, speed, 0, amount);
                } else if (action == "happy") {
                    if (!has_head_) return "错误：没有嘴巴/脖子舵机";
                    QueueAction(ACTION_HAPPY, steps, speed, 0, amount);
                } else {
                    return "错误：无效动作。可用动作：walk, turn, jump, swing, updown, sit, home, "
                           "head_turn, head_shake, look_around, quack, chatter, curious, preen, "
                           "happy_waddle, stretch, dance, peck, yawn, sleepy, "
                           "moonwalk, bend, shake_leg, whirlwind_leg, showcase, "
                           "mouth_open, mouth_close, mouth_move, talk, laugh, sing, think, happy";
                }
                return true;
            });

        mcp_server.AddTool("self.flashduck.stop", "立即停止当前动作并复位", PropertyList(),
                           [this](const PropertyList&) -> ReturnValue {
                               Stop();
                               return true;
                           });

        mcp_server.AddTool("self.flashduck.get_status", "获取 Flashduck 当前状态", PropertyList(),
                           [this](const PropertyList&) -> ReturnValue {
                               return is_action_in_progress_ ? "moving" : "idle";
                           });

        mcp_server.AddTool(
            "self.flashduck.set_trim",
            "设置单个舵机的微调值。servo_type: left_leg/right_leg/left_foot/right_foot/neck/mouth；"
            "trim_value: -50~50 度。",
            PropertyList({Property("servo_type", kPropertyTypeString, "left_leg"),
                          Property("trim_value", kPropertyTypeInteger, 0, -50, 50)}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string servo_type = properties["servo_type"].value<std::string>();
                int trim_value = properties["trim_value"].value<int>();
                Settings settings("flashduck_trims", true);

                int left_leg = settings.GetInt("left_leg", 0);
                int right_leg = settings.GetInt("right_leg", 0);
                int left_foot = settings.GetInt("left_foot", 0);
                int right_foot = settings.GetInt("right_foot", 0);
                int neck = settings.GetInt("neck", 0);
                int mouth = settings.GetInt("mouth", 0);

                if (servo_type == "left_leg") {
                    left_leg = trim_value;
                } else if (servo_type == "right_leg") {
                    right_leg = trim_value;
                } else if (servo_type == "left_foot") {
                    left_foot = trim_value;
                } else if (servo_type == "right_foot") {
                    right_foot = trim_value;
                } else if (servo_type == "neck") {
                    neck = trim_value;
                } else if (servo_type == "mouth") {
                    mouth = trim_value;
                } else {
                    return "错误：servo_type 必须是 left_leg/right_leg/left_foot/right_foot/neck/mouth";
                }

                settings.SetInt("left_leg", left_leg);
                settings.SetInt("right_leg", right_leg);
                settings.SetInt("left_foot", left_foot);
                settings.SetInt("right_foot", right_foot);
                settings.SetInt("neck", neck);
                settings.SetInt("mouth", mouth);
                duck_.SetTrims(left_leg, right_leg, left_foot, right_foot, neck, mouth);
                return "已设置 " + servo_type + " 微调为 " + std::to_string(trim_value) + " 度";
            });

        mcp_server.AddTool("self.flashduck.get_trims", "获取所有舵机微调值", PropertyList(),
                           [](const PropertyList&) -> ReturnValue {
                               Settings settings("flashduck_trims", false);
                               std::string result =
                                   "{\"left_leg\":" + std::to_string(settings.GetInt("left_leg", 0)) +
                                   ",\"right_leg\":" +
                                   std::to_string(settings.GetInt("right_leg", 0)) +
                                   ",\"left_foot\":" +
                                   std::to_string(settings.GetInt("left_foot", 0)) +
                                   ",\"right_foot\":" +
                                   std::to_string(settings.GetInt("right_foot", 0)) +
                                   ",\"neck\":" + std::to_string(settings.GetInt("neck", 0)) +
                                   ",\"mouth\":" + std::to_string(settings.GetInt("mouth", 0)) +
                                   "}";
                               return result;
                           });

        mcp_server.AddTool("self.flashduck.get_ip", "获取 Flashduck 的 WiFi IP", PropertyList(),
                           [](const PropertyList&) -> ReturnValue {
                               auto& wifi = WifiManager::GetInstance();
                               std::string ip = wifi.GetIpAddress();
                               if (ip.empty()) {
                                   return "{\"ip\":\"\",\"connected\":false}";
                               }
                               return "{\"ip\":\"" + ip + "\",\"connected\":true}";
                           });

        mcp_server.AddTool("self.battery.get_level", "获取电池电量和充电状态", PropertyList(),
                           [](const PropertyList&) -> ReturnValue {
                               auto& board = Board::GetInstance();
                               int level = 0;
                               bool charging = false;
                               bool discharging = false;
                               board.GetBatteryLevel(level, charging, discharging);
                               return "{\"level\":" + std::to_string(level) +
                                      ",\"charging\":" + (charging ? "true" : "false") + "}";
                           });

        ESP_LOGI(TAG, "self.flashduck MCP 工具注册完成");
    }
};

static FlashduckController* g_flashduck_controller = nullptr;

void InitializeFlashduckController(const HardwareConfig& hw_config) {
    if (g_flashduck_controller == nullptr) {
        g_flashduck_controller = new FlashduckController(hw_config);
        // 定时提醒：NVS 加载 + 轮询任务；MCP 工具由任务内部延迟注册
        ScheduleManager::GetInstance().Init();
        ESP_LOGI(TAG, "Flashduck 控制器已初始化");
    }
}

void FlashduckStartWalk(int steps, int speed, int direction, int hip_offset, int hip_amp,
                        int foot_amp, int phase_deg, int head_swing) {
    if (g_flashduck_controller != nullptr) {
        g_flashduck_controller->StartWalk(steps, speed, direction, hip_offset, hip_amp, foot_amp,
                                          phase_deg, head_swing);
    }
}

void FlashduckStop() {
    if (g_flashduck_controller != nullptr) {
        g_flashduck_controller->Stop();
    }
}

bool FlashduckIsMoving() {
    return g_flashduck_controller != nullptr && g_flashduck_controller->IsMoving();
}

FlashduckGaitParams FlashduckGetGaitDefaults() {
    if (g_flashduck_controller != nullptr) {
        return g_flashduck_controller->GetGaitDefaults();
    }
    return {FLASHDUCK_WALK_HIP_OFFSET, FLASHDUCK_WALK_HIP_AMP, FLASHDUCK_WALK_FOOT_AMP,
            FLASHDUCK_WALK_PHASE_DEG, FLASHDUCK_WALK_SPEED, FLASHDUCK_WALK_HEAD_SWING};
}

void FlashduckSaveGaitDefaults(const FlashduckGaitParams& gait) {
    if (g_flashduck_controller != nullptr) {
        g_flashduck_controller->SaveGaitDefaults(gait);
    }
}
