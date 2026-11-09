#pragma once

#include "config.h"

struct FlashduckGaitParams {
    int hip_offset;
    int hip_amp;
    int foot_amp;
    int phase_deg;
    int speed;
    int head_swing;
};

void InitializeFlashduckController(const HardwareConfig& hw_config);

// 局域网调参页/测试按钮使用的轻量入口。
void FlashduckStartWalk(int steps, int speed, int direction, int hip_offset, int hip_amp,
                        int foot_amp, int phase_deg, int head_swing);
void FlashduckStop();
bool FlashduckIsMoving();

// 远程指令：按名字执行一个动作（微信/服务端下发）。成功入队返回 true。
bool FlashduckRunAction(const char* action_name);

FlashduckGaitParams FlashduckGetGaitDefaults();
void FlashduckSaveGaitDefaults(const FlashduckGaitParams& gait);
