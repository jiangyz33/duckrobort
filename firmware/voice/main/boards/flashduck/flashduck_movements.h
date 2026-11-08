#ifndef __FLASHDUCK_MOVEMENTS_H__
#define __FLASHDUCK_MOVEMENTS_H__

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "oscillator.h"
#include <atomic>

//-- Constants
#define FORWARD 1
#define BACKWARD -1
#define LEFT 1
#define RIGHT -1
#define BOTH 0
#define SMALL 5
#define MEDIUM 15
#define BIG 30

// -- Servo delta limit default. degree / sec
#define SERVO_LIMIT_DEFAULT 240

// -- 宽站距步态默认参数（Flashduck 腿间距比 Otto 宽，先给一组可调起点）
//    hip_offset: 双腿向身体中线内收的角度；left=+offset, right=-offset
//    hip_amp:    髋部左右摆幅，用于把重心移到支撑脚
//    foot_amp:   脚踝前后摆幅，决定步幅
//    phase_deg:  髋/脚相位差，90 为 Otto 原始值；宽站距可试 95~110
#define FLASHDUCK_WALK_HIP_OFFSET 15
#define FLASHDUCK_WALK_HIP_AMP 20
#define FLASHDUCK_WALK_FOOT_AMP 25
#define FLASHDUCK_WALK_PHASE_DEG 75
#define FLASHDUCK_WALK_SPEED 800
#define FLASHDUCK_WALK_HEAD_SWING 15

// -- Servo indexes for easy access
//    摄像头版机械结构：脖子舵机接 GPIO7（原右手舵机位），嘴巴舵机接 GPIO4（原左手舵机位），腿脚不变
//    代码层索引沿用：NECK=4、MOUTH=5；OttoController.Init 把 right_hand_pin 填给 neck、left_hand_pin 填给 mouth
#define LEFT_LEG 0
#define RIGHT_LEG 1
#define LEFT_FOOT 2
#define RIGHT_FOOT 3
#define NECK 4    // 脖子（物理接 GPIO7）：左右转动，正前方=90°
#define MOUTH 5   // 嘴巴（物理接 GPIO4）：单向开合，闭合=135°
#define SERVO_COUNT 6

// -- 脖子角度约定（静止位=原右手舵机静止位 135°；左右方向装配后若反了，互换 NECK_LEFT_ANGLE/NECK_RIGHT_ANGLE）
//    注意：静止位 135° 偏向 servo 一端，右转受 0–180 限位只能约 +45°；若需左右各 90°，需重装 horn 使静止位=90°
#define NECK_CENTER 135       // 正前方/静止位（原右手舵机静止位）
#define NECK_LEFT_ANGLE 45    // 头转向左侧（135-90）
#define NECK_RIGHT_ANGLE 180  // 头转向右侧（servo 限位，约 +45°）

// -- 嘴巴角度约定（闭合位=原左手舵机静止位 45°；打开=角度增加，从静止位起最多开 15°！）
#define MOUTH_CLOSED 45        // 闭合/静止位（原左手舵机静止位）
#define MOUTH_OPEN_DIR +1      // 打开方向：+1=往大角度（45→60）
#define MOUTH_OPEN_AMOUNT 15   // 从静止位起最大开合行程 15°（绝不可超过！）
#define MOUTH_OPEN (MOUTH_CLOSED + MOUTH_OPEN_DIR * MOUTH_OPEN_AMOUNT)  // 全开位（60）
// 嘴巴物理限位：无论何种代码路径，最终输出角度一律钳在 [45,60]
#define MOUTH_MIN_ANGLE 45
#define MOUTH_MAX_ANGLE 60
// 嘴巴振荡半摆幅上限：中心=开合中点(52°)时，摆幅≤7 才保证不出 [45,60] 限位
#define MOUTH_MAX_SWING ((MOUTH_MAX_ANGLE - MOUTH_MIN_ANGLE) / 2)

class Flashduck {
public:
    Flashduck();
    ~Flashduck();

    //-- Otto initialization
    //   left_hand_pin/right_hand_pin 在摄像头版实际接脖子/嘴巴舵机（字段名沿用历史命名）
    void Init(int left_leg, int right_leg, int left_foot, int right_foot, int neck = -1,
              int mouth = -1);
    //-- Attach & detach functions
    void AttachServos();
    void DetachServos();

    //-- Oscillator Trims
    void SetTrims(int left_leg, int right_leg, int left_foot, int right_foot, int neck = 0,
                  int mouth = 0);

    //-- Predetermined Motion Functions
    void MoveServos(int time, int servo_target[]);
    void MoveSingle(int position, int servo_number);
    void OscillateServos(int amplitude[SERVO_COUNT], int offset[SERVO_COUNT], int period,
                         double phase_diff[SERVO_COUNT], float cycle);
    void Execute2(int amplitude[SERVO_COUNT], int center_angle[SERVO_COUNT], int period,
                  double phase_diff[SERVO_COUNT], float steps);

    //-- HOME = Otto at rest position
    void Home(bool head_reset = true);
    bool GetRestState();
    void SetRestState(bool state);

    //-- Predetermined Motion Functions
    void Jump(float steps = 1, int period = 2000);

    // Walk/Turn 的 head_swing：脖子摆动幅度，0=不摆头（替代原 arm_swing）
    // 后四个参数是宽站距步态调参；不传时使用上面的默认值。
    void Walk(float steps = 4, int period = 1000, int dir = FORWARD, int head_swing = 0,
              int hip_offset = FLASHDUCK_WALK_HIP_OFFSET,
              int hip_amp = FLASHDUCK_WALK_HIP_AMP,
              int foot_amp = FLASHDUCK_WALK_FOOT_AMP,
              int phase_deg = FLASHDUCK_WALK_PHASE_DEG);
    void Turn(float steps = 4, int period = 2000, int dir = LEFT, int head_swing = 0,
              int hip_offset = FLASHDUCK_WALK_HIP_OFFSET,
              int hip_amp = FLASHDUCK_WALK_HIP_AMP,
              int foot_amp = FLASHDUCK_WALK_FOOT_AMP,
              int phase_deg = FLASHDUCK_WALK_PHASE_DEG);
    void Bend(int steps = 1, int period = 1400, int dir = LEFT);
    void ShakeLeg(int steps = 1, int period = 2000, int dir = RIGHT);
    void Sit();  // 坐下

    void UpDown(float steps = 1, int period = 1000, int height = 20);
    void Swing(float steps = 1, int period = 1000, int height = 20);
    void TiptoeSwing(float steps = 1, int period = 900, int height = 20);
    void Jitter(float steps = 1, int period = 500, int height = 20);
    void AscendingTurn(float steps = 1, int period = 900, int height = 20);

    void Moonwalker(float steps = 1, int period = 900, int height = 20, int dir = LEFT);
    void Crusaito(float steps = 1, int period = 900, int height = 20, int dir = FORWARD);
    void Flapping(float steps = 1, int period = 1000, int height = 20, int dir = FORWARD);
    void WhirlwindLeg(float steps = 1, int period = 300, int amplitude = 30);

    // -- 脖子动作
    void HeadTurn(int dir = BOTH, int period = 1000);  // 转头：LEFT/RIGHT/BOTH(正前)
    void HeadShake(float steps = 4, int period = 600, int amplitude = 40);  // 摇头（否定）

    // -- 空闲小动作：脖子轻转 + 嘴巴小幅张合，不改变腿脚
    void IdleLook(int neck_delta, int mouth_cycles, int mouth_amplitude, int move_ms,
                  int open_ms, int close_ms);

    // -- 嘴巴动作
    void MouthOpen(int period = 600);   // 张嘴
    void MouthClose(int period = 600);  // 闭嘴
    void MouthMove(float steps = 6, int period = 400, int amplitude = 15);  // 开合振荡（咀嚼/说话）

    // -- 组合表情动作
    void Talk(float steps = 8, int period = 350, int amplitude = 12);   // 说话
    void Laugh(float steps = 6, int period = 300, int amplitude = 25);  // 大笑
    void Sing(float steps = 10, int period = 500, int amplitude = 20);  // 唱歌

    // -- 扩展动作（脖子可适度幅度，嘴巴≤30，周期偏慢）
    void HeadLookAround(int period = 1200);  // 环顾：左→正→右→正，缓慢
    void Yawn(int period = 1500);            // 打哈欠：慢张嘴+略侧头
    void Think(float steps = 4, int period = 900, int amplitude = 12);  // 思考：侧头+嘴小幅动
    void Happy(float steps = 6, int period = 450, int amplitude = 20);   // 开心：小摇头+嘴开合
    void Sleepy(float steps = 4, int period = 1000, int amplitude = 8);  // 困倦：慢嘴动+慢晃头
    void Showcase();  // 展示动作（串联多个动作）

    // -- Servo limiter
    // 由 otto_action_catalog 生成的每路限位；Init 之后调用就能覆盖掉硬编码。
    void SetJointRange(int index, int min_pos, int max_pos);
    void EnableServoLimit(int speed_limit_degree_per_sec = SERVO_LIMIT_DEFAULT);
    void DisableServoLimit();

    // -- 紧急停止：请求动作循环尽快退出（在 MoveServos/OscillateServos 的循环里检查）。
    //    stop_requested_ 由 stop 动作设置，执行到下一个检查点时提前返回。
    void RequestStop() { stop_requested_ = true; }
    bool IsStopRequested() const { return stop_requested_; }
    void ClearStopRequest() { stop_requested_ = false; }

private:
    std::atomic<bool> stop_requested_{false};
    Oscillator servo_[SERVO_COUNT];

    int servo_pins_[SERVO_COUNT];
    int servo_trim_[SERVO_COUNT];

    unsigned long final_time_;
    unsigned long partial_time_;
    float increment_[SERVO_COUNT];

    bool is_otto_resting_;
    bool has_head_;  // 是否有脖子+嘴巴舵机

    void Execute(int amplitude[SERVO_COUNT], int offset[SERVO_COUNT], int period,
                 double phase_diff[SERVO_COUNT], float steps);

};

#endif  // __FLASHDUCK_MOVEMENTS_H__
