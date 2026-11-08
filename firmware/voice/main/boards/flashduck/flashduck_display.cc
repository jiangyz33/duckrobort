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
// ---- 鍙岀溂甯冨眬锛?40x240 灞忥紝涓ゅ彧 Kawaii 鍦嗙溂锛?---
constexpr int kEyeW = 82;    // 鍗曞彧鐪肩櫧瀹藉害
constexpr int kEyeH = 96;    // 鍗曞彧鐪肩櫧楂樺害锛堢暐楂樼殑鑳跺泭褰㈡洿鍙埍锛?
constexpr int kEyeGap = 24;  // 涓ょ溂涔嬮棿鐨勯棿闅?
constexpr int kEyeTop = 70;  // 璺濆睆骞曢《閮?
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

    // 鐪肩潧灏辨槸涓昏瑙夛紝涓嶅啀鏄剧ず浣庣數閲忓ぇ寮圭獥锛涚數閲忎粛鐢辩姸鎬佹爮鐢垫睜鍥炬爣琛ㄨ揪銆?
    if (low_battery_popup_ != nullptr) {
        lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    }

    auto* dark_theme = LvglThemeManager::GetInstance().GetTheme("dark");
    if (dark_theme != nullptr) {
        SetTheme(dark_theme);
    }

    // 鍋滄帀 GIF/emoji 鍐呭锛屽彧淇濈暀鑷粯鐪肩潧銆?
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

    // 椤堕儴鍙繚鐣欑數姹?缃戠粶绛夊浘鏍囷紝鐘舵€佹枃瀛楀叏閮ㄤ氦缁欑溂鐫涜〃杈俱€?
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
    ESP_LOGI(TAG, "鍗曠溂鍔ㄧ敾宸插惎鍔?);
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
    // 椤堕儴涓嶅啀鏄剧ず浠讳綍鐘舵€佹枃瀛楋紱鎵€鏈夌姸鎬侀兘鏄犲皠鎴愮溂鐫涚鎬併€?
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
    // 琛ㄦ儏杩樺湪灞曠ず涓椂锛屽厛璁颁笅鍩虹绁炴€侊紝绛夊畠鎾畬鍐嶅垏銆?
    if (state_hold_until_ != 0 && static_cast<int32_t>(lv_tick_get() - state_hold_until_) < 0) {
        return;
    }
    SetEyeState(state);
}

void FlashduckDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (notification == nullptr) {
        return;
    }
    // 鐗堟湰銆佹壂鎻?WiFi銆佽繛鎺ヤ腑銆佸凡杩炴帴閮芥敼鎴愮溂绁烇紝涓嶅啀鍗犵敤椤堕儴鏂囧瓧銆?
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
    // 瀛楁椤哄簭锛氫笂鐪肩潙/涓嬬溂鐫?鐬冲瓟缂╂斁/鎵骞呭害x/y/绉诲姩闂撮殧/鐪ㄧ溂闂撮殧/铏硅啘鑹?
    //          楂樺厜缁曡鍗婂緞/楂樺厜瑙掗€熷害/鐬冲瓟鍛煎惛骞呭害
    switch (state) {
        case EyeState::Scanning:
            // 鎵弿/杩炴帴锛氬喎闈掕壊铏硅啘銆佺灣瀛旂缉灏忋€佺溂鐫涚潄澶у苟宸﹀彸鎵锛岄珮鍏夊揩閫熸父璧般€?
            return {0.08f, 0.02f, 0.70f, 12.0f, 3.0f, 900, 1400, 3000, 6500,
                    lv_color_hex(0x2FD0FF), 4.5f, 90.0f, 0.04f};
        case EyeState::Connected:
            // 宸茶繛鎺ワ細缁胯壊铏硅啘 + 鏄庢樉鏈堢墮鐪溂锛岀煭鏆傚仠鐣欏悗鍥炲緟鏈恒€?
            return {0.52f, 0.28f, 1.10f, 2.0f, 1.0f, 2000, 3000, 2500, 5000,
                    lv_color_hex(0x52E37A), 3.0f, 60.0f, 0.03f};
        case EyeState::Listening:
            // 鑱嗗惉锛氱传缃楀叞铏硅啘銆佸畬鍏ㄧ潄澶с€佺灣瀛旀斁鍒版渶澶с€佺溂绁炲畾浣忋€?
            return {0.00f, 0.00f, 1.35f, 3.0f, 2.0f, 1500, 3000, 5000, 9000,
                    lv_color_hex(0x9B7BFF), 5.5f, 45.0f, 0.06f};
        case EyeState::Speaking:
            // 璇磋瘽锛氭鑹茶櫣鑶溿€佺溂鐫涙槑浜€佺溂绁炴洿娲昏穬銆佺湪鐪兼洿棰戠箒銆?
            return {0.10f, 0.02f, 1.15f, 11.0f, 5.0f, 400, 1000, 1600, 3000,
                    lv_color_hex(0xFF5A2A), 6.0f, 150.0f, 0.08f};
        case EyeState::Happy:
            // 寮€蹇冿細寮垚鏈堢墮銆侀噾鍏夈€佺灣瀛旀斁澶с€侀珮鍏夋蹇烦鍔ㄣ€?
            return {0.02f, 0.06f, 1.22f, 10.0f, 6.0f, 500, 1200, 1400, 2800,
                    lv_color_hex(0xFFD24A), 6.0f, 160.0f, 0.08f};
        case EyeState::Curious:
            // 濂藉锛氱潄鍒版渶澶с€佺灣瀛旀敹缂╄仛鐒︺€佹壂瑙嗗箙搴︽渶澶э紝楂樺厜涓€熺粫琛屻€?
            return {0.06f, 0.00f, 1.28f, 14.0f, 9.0f, 500, 1500, 2600, 5200,
                    lv_color_hex(0x2FD0FF), 5.0f, 110.0f, 0.05f};
        case EyeState::Sleepy:
            // 鍥板€︼細鐪肩潙鍘嬪埌涓€鍗娿€佺灣瀛旂暐缂┿€佸姩浣滄瀬鎱€佽櫣鑶滃彂鏆椼€?
            return {0.56f, 0.24f, 0.92f, 4.0f, 2.0f, 2200, 4200, 5200, 9000,
                    lv_color_hex(0x7E8CA8), 2.0f, 18.0f, 0.02f};
        case EyeState::Angry:
            // 鐢熸皵锛氫笂鐫戝帇浣庛€佺灣瀛旂缉绱с€佽櫣鑶滆浆绾€侀珮棰戠煭淇冩壂瑙嗐€?
            return {0.38f, 0.04f, 0.82f, 7.0f, 3.0f, 600, 1200, 2400, 4800,
                    lv_color_hex(0xFF3B30), 2.5f, 70.0f, 0.02f};
        case EyeState::Sad:
            // 闅捐繃锛氱溂鐫戜笅鍨傘€佺溂绁炲線涓嬨€佽櫣鑶滃亸鍐疯摑銆佸姩浣滃緢鎱€?
            return {0.44f, 0.18f, 0.88f, 5.0f, 1.0f, 1800, 3600, 4200, 7800,
                    lv_color_hex(0x6A8BFF), 1.5f, 12.0f, 0.02f};
        case EyeState::Idle:
        default:
            // 寰呮満锛氭煍鍜屽ぉ钃濊櫣鑶溿€佹斁鏉惧井鐪€佹參閫熼殢鏈虹湅銆佸伓灏旂湪鐪笺€?
            return {0.10f, 0.05f, 1.00f, 9.0f, 5.0f, 1400, 3400, 3000, 6500,
                    lv_color_hex(0x4A90E2), 3.5f, 35.0f, 0.03f};
    }
}

FlashduckDisplay::EyeState FlashduckDisplay::StateForEmotion(const char* emotion) {
    if (emotion == nullptr) {
        return EyeState::Idle;
    }
    // 鏈嶅姟绔粠鍥炲閲岀殑 emoji 鎺ㄦ柇鎯呯华锛圗MOJI_MAP锛夛紝杩欓噷鏄犲皠鎴愮溂绁炪€?
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
    ESP_LOGI(TAG, "鐪肩姸鎬?-> %d", static_cast<int>(state));
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
    // 楂樺厜缁曞皬鍦嗘參鎱㈣浆锛岀溂鐝犵湅璧锋潵鏄?婀跨殑銆佹湁绁炵殑"銆?
    sparkle_angle_ += current_style_.sparkle_speed * dt_s;
    if (sparkle_angle_ >= 360.0f) {
        sparkle_angle_ -= 360.0f;
    }
    constexpr float kDeg2Rad = 3.14159265f / 180.0f;
    const float rad = sparkle_angle_ * kDeg2Rad;
    const float amp = std::max(0.0f, current_style_.sparkle_amp);

    // 涓婚珮鍏夛細宸︿笂瑙掑皬骞呯粫琛?+ 鍛煎惛澶у皬
    const int sz = 12 + static_cast<int>(std::lround((std::sin(rad * 1.7f) + 1.0f) * 1.5f));
    const int cx = 17 + static_cast<int>(std::lround(std::cos(rad) * amp));
    const int cy = 15 + static_cast<int>(std::lround(std::sin(rad) * amp));

    // 鍓珮鍏夛細鐩镐綅鐩稿弽锛屽彸涓嬭锛屽皬涓€鐐?
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
    // 鍚姩鏃?Application 浼氭妸 BOARD_NAME/version 浣滀负 system 娑堟伅鎺ㄥ埌鑱婂ぉ鍖猴紱
    // 杩欎笉鏄亰澶╁唴瀹癸紝鐩存帴鍚炴帀銆?
    if (role != nullptr && content != nullptr && strcmp(role, "system") == 0 &&
        IsSystemUserAgent(content)) {
        return;
    }
    SpiLcdDisplay::SetChatMessage(role, content);
}

bool FlashduckDisplay::SetEmotion(const char* emotion) {
    // 鏈嶅姟绔細闅忓洖澶嶄笅鍙戞儏缁悕锛屾槧灏勬垚鐪肩锛?.5 绉掑悗鍥炲埌 SetStatus 缁欑殑鍩虹绁炴€併€?
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
        // 鐖剁被鍦ㄩ瑙堢粨鏉熸椂鍙兘閲嶆柊鎵撳紑 emoji 鍐呭锛岃繖閲岄噸鏂板帇鍥炵溂鐫涖€?
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

    for (int i = 0; i < kEyeCount; ++i) {
        eye_root_[i] = lv_obj_create(screen);
        lv_obj_set_size(eye_root_[i], kEyeW, kEyeH);
        lv_obj_align(eye_root_[i], LV_ALIGN_TOP_MID, (i == 0) ? -x_off : x_off, kEyeTop);
        lv_obj_set_style_bg_opa(eye_root_[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(eye_root_[i], 0, 0);
        lv_obj_set_style_pad_all(eye_root_[i], 0, 0);
        lv_obj_clear_flag(eye_root_[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(eye_root_[i], LV_OBJ_FLAG_CLICKABLE);

        // 鐪肩櫧锛氭贰鍒板嚑涔庣函鐧斤紝灞忓箷涓婃瘮绾櫧鏇存煍鍜?
        sclera_[i] = lv_obj_create(eye_root_[i]);
        lv_obj_set_size(sclera_[i], kEyeW, kEyeH);
        lv_obj_set_pos(sclera_[i], 0, 0);
        lv_obj_set_style_radius(sclera_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sclera_[i], lv_color_hex(0xFAFBFF), 0);
        lv_obj_set_style_bg_opa(sclera_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sclera_[i], 0, 0);
        lv_obj_clear_flag(sclera_[i], LV_OBJ_FLAG_SCROLLABLE);

        // 铏硅啘锛堥鑹查殢鎯呯华鍙樺寲锛?
        iris_[i] = lv_obj_create(sclera_[i]);
        lv_obj_set_size(iris_[i], kIrisSize, kIrisSize);
        lv_obj_set_pos(iris_[i], kIrisOffsetX, kIrisOffsetY);
        lv_obj_set_style_radius(iris_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(iris_[i], lv_color_hex(0x3E8BE8), 0);
        lv_obj_set_style_bg_opa(iris_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(iris_[i], 0, 0);
        lv_obj_clear_flag(iris_[i], LV_OBJ_FLAG_SCROLLABLE);

        pupil_[i] = lv_obj_create(iris_[i]);
        lv_obj_set_size(pupil_[i], kPupilSize, kPupilSize);
        lv_obj_set_pos(pupil_[i], kPupilOffset, kPupilOffset);
        lv_obj_set_style_radius(pupil_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(pupil_[i], lv_color_hex(0x0D1424), 0);
        lv_obj_set_style_bg_opa(pupil_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pupil_[i], 0, 0);
        lv_obj_clear_flag(pupil_[i], LV_OBJ_FLAG_SCROLLABLE);

        // 涓婚珮鍏夛細宸︿笂锛屽ぇ鑰屼寒
        highlight_[i] = lv_obj_create(iris_[i]);
        lv_obj_set_size(highlight_[i], 14, 14);
        lv_obj_set_pos(highlight_[i], 10, 8);
        lv_obj_set_style_radius(highlight_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(highlight_[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(highlight_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(highlight_[i], 0, 0);
        lv_obj_clear_flag(highlight_[i], LV_OBJ_FLAG_SCROLLABLE);

        // 鍓珮鍏夛細鍙充笅锛屽皬鑰屾贰
        highlight2_[i] = lv_obj_create(iris_[i]);
        lv_obj_set_size(highlight2_[i], 8, 8);
        lv_obj_set_pos(highlight2_[i], 34, 34);
        lv_obj_set_style_radius(highlight2_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(highlight2_[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(highlight2_[i], LV_OPA_60, 0);
        lv_obj_set_style_border_width(highlight2_[i], 0, 0);
        lv_obj_clear_flag(highlight2_[i], LV_OBJ_FLAG_SCROLLABLE);

        // 榛戣壊鐪肩潙锛氱湪鐪?/ 鐪溂鏃朵粠涓婁笅鐩栦綇鐪肩櫧
        lid_top_[i] = lv_obj_create(eye_root_[i]);
        lv_obj_set_size(lid_top_[i], kEyeW, 0);
        lv_obj_set_pos(lid_top_[i], 0, 0);
        lv_obj_set_style_radius(lid_top_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(lid_top_[i], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(lid_top_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(lid_top_[i], 0, 0);
        lv_obj_clear_flag(lid_top_[i], LV_OBJ_FLAG_SCROLLABLE);

        lid_bottom_[i] = lv_obj_create(eye_root_[i]);
        lv_obj_set_size(lid_bottom_[i], kEyeW, 0);
        lv_obj_set_pos(lid_bottom_[i], 0, kEyeH);
        lv_obj_set_style_radius(lid_bottom_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(lid_bottom_[i], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(lid_bottom_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(lid_bottom_[i], 0, 0);
        lv_obj_clear_flag(lid_bottom_[i], LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_move_foreground(lid_top_[i]);
        lv_obj_move_foreground(lid_bottom_[i]);
    }

    if (status_bar_ != nullptr) {
        lv_obj_move_foreground(status_bar_);
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

    // 鎯呯华琛ㄦ儏 / 宸茶繛鎺ヨ繖绫荤煭鏆傜鎬侊紝鍒扮偣鍥炲埌 SetStatus 缁欑殑鍩虹鐪肩銆?
    if (state_hold_until_ != 0 && static_cast<int32_t>(now - state_hold_until_) >= 0) {
        state_hold_until_ = 0;
        if (eye_state_ != status_state_) {
            SetEyeState(status_state_);
        }
    }

    // 鐘舵€佷箣闂寸殑鐪肩潙/鐬冲瓟/鎵鑼冨洿骞虫粦杩囨浮锛屼笉纭垏銆?
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
        // 鎵弿 WiFi锛氫笉鏄贡鐪嬶紝鑰屾槸缂撴參宸﹀彸鎵紝鍍忓湪鎵句俊鍙枫€?
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
        // 鑱嗗惉锛氱溂绁炲熀鏈畾浣忥紝鍙仛寰堝皬鐨勫井鎵锛岄厤鍚堟斁澶х殑鐬冲瓟銆?
        if (static_cast<int32_t>(now - next_move_ms_) >= 0) {
            target_x_ = static_cast<float>(static_cast<int>(esp_random() % 5) - 2);
            target_y_ = static_cast<float>(static_cast<int>(esp_random() % 3) - 1);
            next_move_ms_ = now + 1500 + (esp_random() % 2000);
        }
    } else {
        // 寰呮満/璇磋瘽锛氭參閫熼殢鏈烘壂瑙嗭紱璇磋瘽鏃跺弬鏁版洿娲昏穬銆?
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
