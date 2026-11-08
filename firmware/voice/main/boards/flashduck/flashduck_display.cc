#include "flashduck_display.h"

#include <esp_log.h>
#include <esp_random.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "assets/lang_config.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "system_info.h"

#define TAG "FlashduckDisplay"

namespace {
// ---- 双眼布局（240x240 屏，两只 Kawaii 圆眼）----
constexpr int kEyeW = 82;    // 单只眼白宽度
constexpr int kEyeH = 96;    // 单只眼白高度（略高的胶囊形更可爱）
constexpr int kEyeGap = 24;  // 两眼之间的间隙
constexpr int kEyeTop = 34;  // 距屏幕顶部：避开状态栏，也别压到下方聊天区
constexpr int kIrisSize = 54;
constexpr int kIrisOffsetX = (kEyeW - kIrisSize) / 2;
constexpr int kIrisOffsetY = (kEyeH - kIrisSize) / 2;
constexpr int kPupilSize = 28;
constexpr int kPupilOffset = (kIrisSize - kPupilSize) / 2;
constexpr uint32_t kBlinkCloseMs = 80;
constexpr uint32_t kBlinkHoldMs = 35;
constexpr uint32_t kBlinkOpenMs = 170;

bool IsClockText(const char* text) {
    return text != nullptr && strlen(text) == 5 && isdigit(static_cast<unsigned char>(text[0])) &&
           isdigit(static_cast<unsigned char>(text[1])) && text[2] == ':' &&
           isdigit(static_cast<unsigned char>(text[3])) &&
           isdigit(static_cast<unsigned char>(text[4]));
}

bool StartsWithVersion(const char* text) {
    if (text == nullptr) {
        return false;
    }
    const size_t prefix_len = strlen(Lang::Strings::VERSION);
    return strncmp(text, Lang::Strings::VERSION, prefix_len) == 0;
}

bool IsSystemUserAgent(const char* text) {
    return text != nullptr && strcmp(text, SystemInfo::GetUserAgent().c_str()) == 0;
}
}  // namespace

FlashduckDisplay::FlashduckDisplay(esp_lcd_panel_io_handle_t panel_io,
                                   esp_lcd_panel_handle_t panel, int width, int height,
                                   int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                                   bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {}

FlashduckDisplay::~FlashduckDisplay() {
    if (eye_timer_ != nullptr) {
        lv_timer_delete(eye_timer_);
        eye_timer_ = nullptr;
    }
}

void FlashduckDisplay::SetupUI() {
    if (setup_ui_called_) {
        return;
    }

    SpiLcdDisplay::SetupUI();

    // 眼睛就是主视觉，不再显示低电量大弹窗；电量仍由状态栏电池图标表达。
    if (low_battery_popup_ != nullptr) {
        lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    }

    auto* dark_theme = LvglThemeManager::GetInstance().GetTheme("dark");
    if (dark_theme != nullptr) {
        SetTheme(dark_theme);
    }

    // 停掉 GIF/emoji 内容，只保留自绘眼睛。
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (emoji_image_ != nullptr) {
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    if (preview_image_ != nullptr) {
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    }

    // 顶部只保留电池/网络等图标，状态文字全部交给眼睛表达。
    if (status_label_ != nullptr) {
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }

    CreateEye();

    if (!eye_state_set_) {
        SetEyeState(EyeState::Idle);
    }
    current_style_ = target_style_;
    ApplyPupil();
    ApplyIrisColor();
    ApplyLids(0.0f);

    uint32_t now = lv_tick_get();
    next_move_ms_ = now + 500;
    next_blink_ms_ = now + target_style_.blink_min_ms +
                     (esp_random() % (target_style_.blink_max_ms - target_style_.blink_min_ms + 1));
    eye_timer_ = lv_timer_create(OnEyeTimer, 30, this);
    ESP_LOGI(TAG, "双眼动画已启动");
}

void FlashduckDisplay::HideStatusLabel() {
    if (!setup_ui_called_ || status_label_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
}

void FlashduckDisplay::SetStatus(const char* status) {
    if (status == nullptr) {
        return;
    }
    // 顶部不再显示任何状态文字；所有状态都映射成眼睛神态。
    HideStatusLabel();

    if (IsClockText(status) || StartsWithVersion(status) || IsSystemUserAgent(status)) {
        return;
    }

    if (strcmp(status, Lang::Strings::STANDBY) == 0) {
        SelectStatusState(EyeState::Idle);
    } else if (strcmp(status, Lang::Strings::LISTENING) == 0) {
        SelectStatusState(EyeState::Listening);
    } else if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
        SelectStatusState(EyeState::Speaking);
    } else if (strcmp(status, Lang::Strings::CONNECTING) == 0 ||
               strcmp(status, Lang::Strings::SCANNING_WIFI) == 0 ||
               strcmp(status, Lang::Strings::WIFI_CONFIG_MODE) == 0 ||
               strcmp(status, Lang::Strings::REGISTERING_NETWORK) == 0 ||
               strcmp(status, Lang::Strings::DETECTING_MODULE) == 0) {
        SelectStatusState(EyeState::Scanning);
    } else if (strcmp(status, Lang::Strings::ACTIVATION) == 0 ||
               strcmp(status, Lang::Strings::LOADING_PROTOCOL) == 0 ||
               strcmp(status, Lang::Strings::CHECKING_NEW_VERSION) == 0) {
        SelectStatusState(EyeState::Connected);
    } else if (strncmp(status, Lang::Strings::CONNECTED_TO,
                       strlen(Lang::Strings::CONNECTED_TO)) == 0) {
        SelectStatusState(EyeState::Connected);
    } else if (strncmp(status, Lang::Strings::CONNECT_TO,
                       strlen(Lang::Strings::CONNECT_TO)) == 0) {
        SelectStatusState(EyeState::Scanning);
    }
}

void FlashduckDisplay::SelectStatusState(EyeState state) {
    status_state_ = state;
    // 表情还在展示中时，先记下基础神态，等它播完再切。
    if (state_hold_until_ != 0 && static_cast<int32_t>(lv_tick_get() - state_hold_until_) < 0) {
        return;
    }
    SetEyeState(state);
}

void FlashduckDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (notification == nullptr) {
        return;
    }
    // 版本、扫描 WiFi、连接中、已连接都改成眼神，不再占用顶部文字。
    if (StartsWithVersion(notification)) {
        return;
    }
    if (strncmp(notification, Lang::Strings::CONNECTED_TO,
                strlen(Lang::Strings::CONNECTED_TO)) == 0) {
        SetEyeState(EyeState::Connected);
        return;
    }
    if (strcmp(notification, Lang::Strings::SCANNING_WIFI) == 0 ||
        strncmp(notification, Lang::Strings::CONNECT_TO,
                strlen(Lang::Strings::CONNECT_TO)) == 0) {
        SetEyeState(EyeState::Scanning);
        return;
    }
    SpiLcdDisplay::ShowNotification(notification, duration_ms);
}

FlashduckDisplay::EyeStyle FlashduckDisplay::StyleFor(EyeState state) {
    // 字段顺序：上眼睑/下眼睑/瞳孔缩放/扫视幅度x/y/移动间隔/眨眼间隔/虹膜色/
    //          高光绕行半径/高光角速度/瞳孔呼吸幅度
    switch (state) {
        case EyeState::Scanning:
            // 扫描/连接：冷青色虹膜、瞳孔缩小、眼睛睁大并左右扫视，高光快速游走。
            return {0.08f, 0.02f, 0.70f, 12.0f, 3.0f, 900, 1400, 3000, 6500,
                    lv_color_hex(0x2FD0FF), 4.5f, 90.0f, 0.04f};
        case EyeState::Connected:
            // 已连接：绿色虹膜 + 明显月牙眯眼，短暂停留后回待机。
            return {0.52f, 0.28f, 1.10f, 2.0f, 1.0f, 2000, 3000, 2500, 5000,
                    lv_color_hex(0x52E37A), 3.0f, 60.0f, 0.03f};
        case EyeState::Listening:
            // 聆听：紫罗兰虹膜、完全睁大、瞳孔放到最大、眼神定住。
            return {0.00f, 0.00f, 1.35f, 3.0f, 2.0f, 1500, 3000, 5000, 9000,
                    lv_color_hex(0x9B7BFF), 5.5f, 45.0f, 0.06f};
        case EyeState::Speaking:
            // 说话：橙色虹膜、眼睛明亮、眼神更活跃、眨眼更频繁。
            return {0.10f, 0.02f, 1.15f, 11.0f, 5.0f, 400, 1000, 1600, 3000,
                    lv_color_hex(0xFF5A2A), 6.0f, 150.0f, 0.08f};
        case EyeState::Happy:
            // 开心：弯成月牙、金光、瞳孔放大、高光欢快跳动。
            return {0.02f, 0.06f, 1.22f, 10.0f, 6.0f, 500, 1200, 1400, 2800,
                    lv_color_hex(0xFFD24A), 6.0f, 160.0f, 0.08f};
        case EyeState::Curious:
            // 好奇：睁到最大、瞳孔收缩聚焦、扫视幅度最大，高光中速绕行。
            return {0.06f, 0.00f, 1.28f, 14.0f, 9.0f, 500, 1500, 2600, 5200,
                    lv_color_hex(0x2FD0FF), 5.0f, 110.0f, 0.05f};
        case EyeState::Sleepy:
            // 困倦：眼睑压到一半、瞳孔略缩、动作极慢、虹膜发暗。
            return {0.56f, 0.24f, 0.92f, 4.0f, 2.0f, 2200, 4200, 5200, 9000,
                    lv_color_hex(0x7E8CA8), 2.0f, 18.0f, 0.02f};
        case EyeState::Angry:
            // 生气：上睑压低、瞳孔缩紧、虹膜转红、高频短促扫视。
            return {0.38f, 0.04f, 0.82f, 7.0f, 3.0f, 600, 1200, 2400, 4800,
                    lv_color_hex(0xFF3B30), 2.5f, 70.0f, 0.02f};
        case EyeState::Sad:
            // 难过：眼睑下垂、眼神往下、虹膜偏冷蓝、动作很慢。
            return {0.44f, 0.18f, 0.88f, 5.0f, 1.0f, 1800, 3600, 4200, 7800,
                    lv_color_hex(0x6A8BFF), 1.5f, 12.0f, 0.02f};
        case EyeState::Idle:
        default:
            // 待机：柔和天蓝虹膜、放松微眯、慢速随机看、偶尔眨眼。
            return {0.10f, 0.05f, 1.00f, 9.0f, 5.0f, 1400, 3400, 3000, 6500,
                    lv_color_hex(0x4A90E2), 3.5f, 35.0f, 0.03f};
    }
}

FlashduckDisplay::EyeState FlashduckDisplay::StateForEmotion(const char* emotion) {
    if (emotion == nullptr) {
        return EyeState::Idle;
    }
    // 服务端从回复里的 emoji 推断情绪（EMOJI_MAP），这里映射成眼神。
    if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "laughing") == 0 ||
        strcmp(emotion, "funny") == 0 || strcmp(emotion, "delicious") == 0 ||
        strcmp(emotion, "kissy") == 0 || strcmp(emotion, "confident") == 0 ||
        strcmp(emotion, "loving") == 0 || strcmp(emotion, "winking") == 0) {
        return EyeState::Happy;
    }
    if (strcmp(emotion, "sad") == 0 || strcmp(emotion, "crying") == 0) {
        return EyeState::Sad;
    }
    if (strcmp(emotion, "angry") == 0) {
        return EyeState::Angry;
    }
    if (strcmp(emotion, "sleepy") == 0 || strcmp(emotion, "relaxed") == 0) {
        return EyeState::Sleepy;
    }
    if (strcmp(emotion, "thinking") == 0 || strcmp(emotion, "confused") == 0 ||
        strcmp(emotion, "surprised") == 0 || strcmp(emotion, "shocked") == 0 ||
        strcmp(emotion, "curious") == 0 || strcmp(emotion, "silly") == 0 ||
        strcmp(emotion, "embarrassed") == 0) {
        return EyeState::Curious;
    }
    return EyeState::Idle;
}

void FlashduckDisplay::SetEyeState(EyeState state) {
    const uint32_t now = lv_tick_get();
    if (eye_state_ == state) {
        if (state == EyeState::Connected) {
            state_hold_until_ = now + 2200;
        }
        return;
    }

    eye_state_ = state;
    eye_state_set_ = true;
    ESP_LOGI(TAG, "眼状态 -> %d", static_cast<int>(state));
    target_style_ = StyleFor(state);
    state_hold_until_ = state == EyeState::Connected ? now + 2200 : 0;
    next_scan_ms_ = now + 200;
    next_move_ms_ = now + 250;
    target_x_ = 0.0f;
    target_y_ = 0.0f;

    const uint32_t blink_span = target_style_.blink_max_ms - target_style_.blink_min_ms + 1;
    next_blink_ms_ = now + target_style_.blink_min_ms + (esp_random() % blink_span);
}

void FlashduckDisplay::ApplyPupil() {
    const float scale = std::clamp(
        current_style_.pupil_scale * (1.0f + current_style_.pupil_breathe * std::sin(breathe_phase_)),
        0.60f, 1.45f);
    const int size = static_cast<int>(kPupilSize * scale + 0.5f);
    const int offset = (kIrisSize - size) / 2;
    for (int i = 0; i < kEyeCount; ++i) {
        if (pupil_[i] == nullptr) {
            continue;
        }
        lv_obj_set_size(pupil_[i], size, size);
        lv_obj_set_pos(pupil_[i], offset, offset);
    }
}

void FlashduckDisplay::ApplyIrisColor() {
    for (int i = 0; i < kEyeCount; ++i) {
        if (iris_[i] == nullptr) {
            continue;
        }
        lv_obj_set_style_bg_color(iris_[i], current_style_.iris_color, 0);
    }
}

void FlashduckDisplay::ApplySparkle(uint32_t now, float dt_s) {
    (void)now;
    // 高光绕小圆慢慢转，眼珠看起来是"湿的、有神的"。
    sparkle_angle_ += current_style_.sparkle_speed * dt_s;
    if (sparkle_angle_ >= 360.0f) {
        sparkle_angle_ -= 360.0f;
    }
    constexpr float kDeg2Rad = 3.14159265f / 180.0f;
    const float rad = sparkle_angle_ * kDeg2Rad;
    const float amp = std::max(0.0f, current_style_.sparkle_amp);

    // 主高光：左上角小幅绕行 + 呼吸大小
    const int sz = 12 + static_cast<int>(std::lround((std::sin(rad * 1.7f) + 1.0f) * 1.5f));
    const int cx = 17 + static_cast<int>(std::lround(std::cos(rad) * amp));
    const int cy = 15 + static_cast<int>(std::lround(std::sin(rad) * amp));

    // 副高光：相位相反，右下角，小一点
    const int sz2 = 6 + static_cast<int>(std::lround((std::sin(rad * 1.3f + 1.9f) + 1.0f) * 0.8f));
    const int cx2 = 38 - static_cast<int>(std::lround(std::cos(rad) * amp * 0.55f));
    const int cy2 = 38 - static_cast<int>(std::lround(std::sin(rad) * amp * 0.55f));

    const int hx = std::clamp(cx, sz / 2 + 1, kIrisSize - sz / 2 - 1);
    const int hy = std::clamp(cy, sz / 2 + 1, kIrisSize - sz / 2 - 1);
    const int hx2 = std::clamp(cx2, sz2 / 2 + 1, kIrisSize - sz2 / 2 - 1);
    const int hy2 = std::clamp(cy2, sz2 / 2 + 1, kIrisSize - sz2 / 2 - 1);

    for (int i = 0; i < kEyeCount; ++i) {
        if (highlight_[i] != nullptr) {
            lv_obj_set_size(highlight_[i], sz, sz);
            lv_obj_set_pos(highlight_[i], hx - sz / 2, hy - sz / 2);
        }
        if (highlight2_[i] != nullptr) {
            lv_obj_set_size(highlight2_[i], sz2, sz2);
            lv_obj_set_pos(highlight2_[i], hx2 - sz2 / 2, hy2 - sz2 / 2);
        }
    }
}

int FlashduckDisplay::ApplyLids(float blink_fraction) {
    const float top_base = std::clamp(current_style_.upper_lid, 0.0f, 0.85f);
    const float bottom_base = std::clamp(current_style_.lower_lid, 0.0f, 0.60f);
    const float f = std::clamp(blink_fraction, 0.0f, 1.0f);
    const float top = top_base + (1.0f - top_base) * f;
    const float bottom = bottom_base + (1.0f - bottom_base) * f * 0.15f;
    const int top_height = static_cast<int>(kEyeH * top + 0.5f);
    const int bottom_height = static_cast<int>(kEyeH * bottom + 0.5f);
    for (int i = 0; i < kEyeCount; ++i) {
        if (lid_top_[i] != nullptr) {
            lv_obj_set_height(lid_top_[i], top_height);
            lv_obj_set_pos(lid_top_[i], 0, 0);
        }
        if (lid_bottom_[i] != nullptr) {
            lv_obj_set_height(lid_bottom_[i], bottom_height);
            lv_obj_set_pos(lid_bottom_[i], 0, kEyeH - bottom_height);
        }
    }
    return top_height;
}

void FlashduckDisplay::SetChatMessage(const char* role, const char* content) {
    // 启动时 Application 会把 BOARD_NAME/version 作为 system 消息推到聊天区；
    // 这不是聊天内容，直接吞掉。
    if (role != nullptr && content != nullptr && strcmp(role, "system") == 0 &&
        IsSystemUserAgent(content)) {
        return;
    }
    SpiLcdDisplay::SetChatMessage(role, content);
}

bool FlashduckDisplay::SetEmotion(const char* emotion) {
    // 服务端会随回复下发情绪名，映射成眼神；2.5 秒后回到 SetStatus 给的基础神态。
    const EyeState s = StateForEmotion(emotion);
    if (s == EyeState::Idle) {
        if (eye_state_ != status_state_ &&
            (state_hold_until_ == 0 ||
             static_cast<int32_t>(lv_tick_get() - state_hold_until_) >= 0)) {
            SetEyeState(status_state_);
        }
        return true;
    }
    if (eye_state_ != s) {
        SetEyeState(s);
    }
    state_hold_until_ = lv_tick_get() + 2500;
    return true;
}

void FlashduckDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    const bool show_eye = (image == nullptr);
    {
        DisplayLockGuard lock(this);
        for (int i = 0; i < kEyeCount; ++i) {
            if (eye_root_[i] == nullptr) {
                continue;
            }
            if (show_eye) {
                lv_obj_remove_flag(eye_root_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(eye_root_[i]);
            } else {
                lv_obj_add_flag(eye_root_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (eye_timer_ != nullptr) {
            if (show_eye) {
                lv_timer_resume(eye_timer_);
            } else {
                lv_timer_pause(eye_timer_);
            }
        }
    }

    SpiLcdDisplay::SetPreviewImage(std::move(image));

    if (show_eye) {
        DisplayLockGuard lock(this);
        // 父类在预览结束时可能重新打开 emoji 内容，这里重新压回眼睛。
        if (gif_controller_) {
            gif_controller_->Stop();
        }
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_image_ != nullptr) {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_label_ != nullptr) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        for (int i = 0; i < kEyeCount; ++i) {
            if (eye_root_[i] != nullptr) {
                lv_obj_remove_flag(eye_root_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(eye_root_[i]);
            }
        }
        if (status_bar_ != nullptr) {
            lv_obj_move_foreground(status_bar_);
        }
    }
}

void FlashduckDisplay::UpdateStatusBar(bool update_all) {
    SpiLcdDisplay::UpdateStatusBar(update_all);
    if (low_battery_popup_ != nullptr) {
        lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    }
}

void FlashduckDisplay::CreateEye() {
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    const int x_off = kEyeGap / 2 + kEyeW / 2;

    // 统一清掉样式：默认主题给 lv_obj 带的 padding / border 会让子对象
    // 的坐标从"内容区"起算，导致虹膜整体偏移（实测偏右下）。
    auto prep = [](lv_obj_t* o) {
        lv_obj_set_style_pad_all(o, 0, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    };

    for (int i = 0; i < kEyeCount; ++i) {
        eye_root_[i] = lv_obj_create(screen);
        lv_obj_set_size(eye_root_[i], kEyeW, kEyeH);
        lv_obj_align(eye_root_[i], LV_ALIGN_TOP_MID, (i == 0) ? -x_off : x_off, kEyeTop);
        lv_obj_set_style_bg_opa(eye_root_[i], LV_OPA_TRANSP, 0);
        prep(eye_root_[i]);
        lv_obj_clear_flag(eye_root_[i], LV_OBJ_FLAG_CLICKABLE);

        // 眼白
        sclera_[i] = lv_obj_create(eye_root_[i]);
        lv_obj_set_size(sclera_[i], kEyeW, kEyeH);
        lv_obj_set_pos(sclera_[i], 0, 0);
        lv_obj_set_style_radius(sclera_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sclera_[i], lv_color_hex(0xFAFBFF), 0);
        lv_obj_set_style_bg_opa(sclera_[i], LV_OPA_COVER, 0);
        prep(sclera_[i]);

        // 虹膜（颜色随情绪变化）
        iris_[i] = lv_obj_create(sclera_[i]);
        lv_obj_set_size(iris_[i], kIrisSize, kIrisSize);
        lv_obj_set_pos(iris_[i], kIrisOffsetX, kIrisOffsetY);
        lv_obj_set_style_radius(iris_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(iris_[i], lv_color_hex(0x3E8BE8), 0);
        lv_obj_set_style_bg_opa(iris_[i], LV_OPA_COVER, 0);
        prep(iris_[i]);

        pupil_[i] = lv_obj_create(iris_[i]);
        lv_obj_set_size(pupil_[i], kPupilSize, kPupilSize);
        lv_obj_set_pos(pupil_[i], kPupilOffset, kPupilOffset);
        lv_obj_set_style_radius(pupil_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(pupil_[i], lv_color_hex(0x0D1424), 0);
        lv_obj_set_style_bg_opa(pupil_[i], LV_OPA_COVER, 0);
        prep(pupil_[i]);

        // 主高光：左上，大而亮
        highlight_[i] = lv_obj_create(iris_[i]);
        lv_obj_set_size(highlight_[i], 14, 14);
        lv_obj_set_pos(highlight_[i], 10, 8);
        lv_obj_set_style_radius(highlight_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(highlight_[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(highlight_[i], LV_OPA_COVER, 0);
        prep(highlight_[i]);

        // 副高光：右下，小而淡
        highlight2_[i] = lv_obj_create(iris_[i]);
        lv_obj_set_size(highlight2_[i], 8, 8);
        lv_obj_set_pos(highlight2_[i], 34, 34);
        lv_obj_set_style_radius(highlight2_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(highlight2_[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(highlight2_[i], LV_OPA_60, 0);
        prep(highlight2_[i]);

        // 黑色眼睑：眨眼 / 眯眼时从上下盖住眼白
        lid_top_[i] = lv_obj_create(eye_root_[i]);
        lv_obj_set_size(lid_top_[i], kEyeW, 0);
        lv_obj_set_pos(lid_top_[i], 0, 0);
        lv_obj_set_style_radius(lid_top_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(lid_top_[i], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(lid_top_[i], LV_OPA_COVER, 0);
        prep(lid_top_[i]);

        lid_bottom_[i] = lv_obj_create(eye_root_[i]);
        lv_obj_set_size(lid_bottom_[i], kEyeW, 0);
        lv_obj_set_pos(lid_bottom_[i], 0, kEyeH);
        lv_obj_set_style_radius(lid_bottom_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(lid_bottom_[i], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(lid_bottom_[i], LV_OPA_COVER, 0);
        prep(lid_bottom_[i]);

        lv_obj_move_foreground(lid_top_[i]);
        lv_obj_move_foreground(lid_bottom_[i]);
    }

    if (status_bar_ != nullptr) {
        lv_obj_move_foreground(status_bar_);
    }
    // 眼睛压到最上层，避免被聊天区/提示文字盖住
    for (int i = 0; i < kEyeCount; ++i) {
        if (eye_root_[i] != nullptr) {
            lv_obj_move_foreground(eye_root_[i]);
        }
    }
}

void FlashduckDisplay::OnEyeTimer(lv_timer_t* timer) {
    auto* self = static_cast<FlashduckDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->UpdateEye();
    }
}

void FlashduckDisplay::UpdateEye() {
    if (eye_root_[0] == nullptr || iris_[0] == nullptr) {
        return;
    }

    const uint32_t now = lv_tick_get();
    const float dt_s = (last_frame_ms_ == 0)
                           ? 0.033f
                           : std::clamp(static_cast<float>(now - last_frame_ms_) / 1000.0f, 0.0f, 0.1f);
    last_frame_ms_ = now;
    breathe_phase_ += dt_s * 2.4f;
    if (breathe_phase_ > 6.2831853f) {
        breathe_phase_ -= 6.2831853f;
    }

    // 情绪表情 / 已连接这类短暂神态，到点回到 SetStatus 给的基础眼神。
    if (state_hold_until_ != 0 && static_cast<int32_t>(now - state_hold_until_) >= 0) {
        state_hold_until_ = 0;
        if (eye_state_ != status_state_) {
            SetEyeState(status_state_);
        }
    }

    // 状态之间的眼睑/瞳孔/扫视范围平滑过渡，不硬切。
    constexpr float kStyleLerp = 0.08f;
    current_style_.upper_lid += (target_style_.upper_lid - current_style_.upper_lid) * kStyleLerp;
    current_style_.lower_lid += (target_style_.lower_lid - current_style_.lower_lid) * kStyleLerp;
    current_style_.pupil_scale +=
        (target_style_.pupil_scale - current_style_.pupil_scale) * kStyleLerp;
    current_style_.gaze_x += (target_style_.gaze_x - current_style_.gaze_x) * kStyleLerp;
    current_style_.gaze_y += (target_style_.gaze_y - current_style_.gaze_y) * kStyleLerp;
    current_style_.sparkle_amp +=
        (target_style_.sparkle_amp - current_style_.sparkle_amp) * kStyleLerp;
    current_style_.sparkle_speed +=
        (target_style_.sparkle_speed - current_style_.sparkle_speed) * kStyleLerp;
    current_style_.pupil_breathe +=
        (target_style_.pupil_breathe - current_style_.pupil_breathe) * kStyleLerp;
    auto lerp_channel = [](uint8_t from, uint8_t to) -> uint8_t {
        return static_cast<uint8_t>(from + (static_cast<int>(to) - static_cast<int>(from)) * 0.08f);
    };
    current_style_.iris_color.red =
        lerp_channel(current_style_.iris_color.red, target_style_.iris_color.red);
    current_style_.iris_color.green =
        lerp_channel(current_style_.iris_color.green, target_style_.iris_color.green);
    current_style_.iris_color.blue =
        lerp_channel(current_style_.iris_color.blue, target_style_.iris_color.blue);
    ApplyPupil();
    ApplyIrisColor();

    if (eye_state_ == EyeState::Scanning) {
        // 扫描 WiFi：不是乱看，而是缓慢左右扫，像在找信号。
        if (static_cast<int32_t>(now - next_scan_ms_) >= 0) {
            scan_right_ = !scan_right_;
            target_x_ = (scan_right_ ? 1.0f : -1.0f) * current_style_.gaze_x;
            target_y_ = 0.0f;
            next_scan_ms_ = now + 800 + (esp_random() % 500);
        }
    } else if (eye_state_ == EyeState::Connected) {
        target_x_ = 0.0f;
        target_y_ = 0.0f;
    } else if (eye_state_ == EyeState::Listening) {
        // 聆听：眼神基本定住，只做很小的微扫视，配合放大的瞳孔。
        if (static_cast<int32_t>(now - next_move_ms_) >= 0) {
            target_x_ = static_cast<float>(static_cast<int>(esp_random() % 5) - 2);
            target_y_ = static_cast<float>(static_cast<int>(esp_random() % 3) - 1);
            next_move_ms_ = now + 1500 + (esp_random() % 2000);
        }
    } else {
        // 待机/说话：慢速随机扫视；说话时参数更活跃。
        if (static_cast<int32_t>(now - next_move_ms_) >= 0) {
            const int gx = std::max(1, static_cast<int>(current_style_.gaze_x));
            const int gy = std::max(1, static_cast<int>(current_style_.gaze_y));
            int tx = 0;
            int ty = 0;
            if ((esp_random() % 100) >= 18) {
                tx = static_cast<int>(esp_random() % (2 * gx + 1)) - gx;
                ty = static_cast<int>(esp_random() % (2 * gy + 1)) - gy;
            }
            target_x_ = static_cast<float>(tx);
            target_y_ = static_cast<float>(ty);
            const uint32_t span =
                target_style_.move_max_ms > target_style_.move_min_ms
                    ? target_style_.move_max_ms - target_style_.move_min_ms
                    : 1;
            next_move_ms_ = now + target_style_.move_min_ms + (esp_random() % span);
        }
    }

    const float max_x = std::max(1.0f, current_style_.gaze_x);
    const float max_y = std::max(1.0f, current_style_.gaze_y);
    target_x_ = std::clamp(target_x_, -max_x, max_x);
    target_y_ = std::clamp(target_y_, -max_y, max_y);
    current_x_ += (target_x_ - current_x_) * 0.16f;
    current_y_ += (target_y_ - current_y_) * 0.16f;
    const int gaze_dx = static_cast<int>(std::lround(current_x_));
    const int gaze_dy = static_cast<int>(std::lround(current_y_));
    for (int i = 0; i < kEyeCount; ++i) {
        if (iris_[i] != nullptr) {
            lv_obj_set_pos(iris_[i], kIrisOffsetX + gaze_dx, kIrisOffsetY + gaze_dy);
        }
    }

    float blink_fraction = 0.0f;
    if (!blinking_ && static_cast<int32_t>(now - next_blink_ms_) >= 0) {
        blinking_ = true;
        blink_start_ms_ = now;
        double_blink_ = (esp_random() % 100) < 12;
        const uint32_t span =
            target_style_.blink_max_ms > target_style_.blink_min_ms
                ? target_style_.blink_max_ms - target_style_.blink_min_ms
                : 1;
        next_blink_ms_ = now + target_style_.blink_min_ms + (esp_random() % span);
    }

    if (blinking_) {
        const uint32_t elapsed = now - blink_start_ms_;
        float close_fraction = 0.0f;
        bool finished = false;

        if (elapsed < kBlinkCloseMs) {
            close_fraction = static_cast<float>(elapsed) / kBlinkCloseMs;
        } else if (elapsed < kBlinkCloseMs + kBlinkHoldMs) {
            close_fraction = 1.0f;
        } else if (elapsed < kBlinkCloseMs + kBlinkHoldMs + kBlinkOpenMs) {
            close_fraction =
                1.0f - static_cast<float>(elapsed - kBlinkCloseMs - kBlinkHoldMs) / kBlinkOpenMs;
        } else {
            finished = true;
        }

        if (finished) {
            blinking_ = false;
            if (double_blink_) {
                double_blink_ = false;
                next_blink_ms_ = now + 180;
            }
        } else {
            blink_fraction = close_fraction * close_fraction * (3.0f - 2.0f * close_fraction);
        }
    }

    ApplyLids(blink_fraction);
    ApplySparkle(now, dt_s);
}
