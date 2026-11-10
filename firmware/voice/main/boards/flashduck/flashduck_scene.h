#pragma once

#include <string>

/// 定时场景感知：隔一段时间拍一张，让模型描述画面，存到服务端。
///
/// 挂载在 ScheduleManager 的 1 秒 tick 里执行（不新建任务 —— 之前板级初始化
/// 阶段创建任务会把启动搞挂）。为了不拖慢提醒，拍照间隔按设备状态区分：
///   - 待机：10 分钟一张
///   - 对话中（聆听/说话/连接）：20 秒一张
void FlashduckSceneTick();

/// 读取服务端保存的最近画面描述（给 MCP 工具用）。返回空串表示读取失败。
std::string FlashduckSceneRecent(int limit);
