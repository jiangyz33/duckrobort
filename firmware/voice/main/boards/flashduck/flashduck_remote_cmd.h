#pragma once

/// 启动远程指令轮询任务：定时向服务端要「微信里下达的指令」并执行。
///
/// 为什么不常连：设备只在唤醒后和服务器建立 WebSocket，
/// 空闲时服务器够不到它；所以改成设备主动来取。
void StartFlashduckRemoteCmdPoller();
