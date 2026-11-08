#pragma once

#include "display/lcd_display.h"

/**
 * Flashduck 的单眼显示：屏幕上方 1/2 中间一个圆形眼睛。
 * 瞳孔会随机扫视，眼睑随机眨动；不依赖 GIF/emoji 素材。
 */
class FlashduckDisplay : public SpiLcdDisplay {
public:
    FlashduckDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                     int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                     bool swap_xy);
    ~FlashduckDisplay() override;

    void SetupUI() override;
    void SetStatus(const char* status) override;
    bool SetEmotion(const char* emotion) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    void UpdateStatusBar(bool update_all = false) override;

private:
    enum class EyeState {
        Idle,
        Scanning,
        Connected,
        Listening,
        Speaking,
        Happy,
        Curious,
        Sleepy,
        Angry,
        Sad,
    };

    struct EyeStyle {
        float upper_lid;
        float lower_lid;
        float pupil_scale;
        float gaze_x;
        float gaze_y;
        uint32_t move_min_ms;
        uint32_t move_max_ms;
        uint32_t blink_min_ms;
        uint32_t blink_max_ms;
        lv_color_t iris_color;
        float sparkle_amp;      // 高光绕行半径(px)，0=不动
        float sparkle_speed;    // 高光绕行角速度(度/秒)
        float pupil_breathe;    // 瞳孔呼吸幅度(0~0.15)
    };

    void CreateEye();
    void UpdateEye();
    void SetEyeState(EyeState state);
    int ApplyLids(float blink_fraction);
    void ApplyPupil();
    void ApplyIrisColor();
    void ApplySparkle(uint32_t now, float dt_s);
    void ApplyLashes(int top_height);
    void HideStatusLabel();
    void SelectStatusState(EyeState state);
    static EyeState StateForEmotion(const char* emotion);
    static EyeStyle StyleFor(EyeState state);
    static void OnEyeTimer(lv_timer_t* timer);

    lv_obj_t* eye_root_ = nullptr;
    lv_obj_t* sclera_ = nullptr;
    lv_obj_t* iris_ = nullptr;
    lv_obj_t* pupil_ = nullptr;
    lv_obj_t* highlight_ = nullptr;
    lv_obj_t* highlight2_ = nullptr;
    lv_obj_t* lash_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* lid_top_ = nullptr;
    lv_obj_t* lid_bottom_ = nullptr;
    lv_timer_t* eye_timer_ = nullptr;

    uint32_t next_move_ms_ = 0;
    uint32_t next_blink_ms_ = 0;
    uint32_t blink_start_ms_ = 0;
    bool blinking_ = false;
    bool double_blink_ = false;
    float target_x_ = 0.0f;
    float target_y_ = 0.0f;
    float current_x_ = 0.0f;
    float current_y_ = 0.0f;

    EyeState eye_state_ = EyeState::Idle;
    EyeState status_state_ = EyeState::Idle;  // SetStatus 给的基础神态，表情播完回它
    bool eye_state_set_ = false;
    EyeStyle target_style_{};
    EyeStyle current_style_{};
    uint32_t state_hold_until_ = 0;
    uint32_t next_scan_ms_ = 0;
    uint32_t last_frame_ms_ = 0;
    float sparkle_angle_ = 0.0f;
    float breathe_phase_ = 0.0f;
    bool scan_right_ = true;
};
