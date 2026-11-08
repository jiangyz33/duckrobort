#pragma once

#include <esp_timer.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

/**
 * 唇形同步电平
 *
 * 音频输出层每送出一块 PCM 就发布一次"这块的响度"，
 * 舵机侧据此驱动嘴巴开合。
 *
 * 关键点：只有在真正要出声时才有非零值，所以不会出现
 * "先张嘴、TTS 后出声"的错位——之前那版是按设备状态猜的。
 *
 * 用 inline 函数里的局部 static，跨编译单元共享同一份实例。
 */
namespace LipSync {

inline std::atomic<uint8_t>& LevelSlot() {
    static std::atomic<uint8_t> slot{0};
    return slot;
}

/// 最后一次发布的时间戳（毫秒）
inline std::atomic<uint32_t>& StampSlot() {
    static std::atomic<uint32_t> stamp{0};
    return stamp;
}

/// 当前响度 0~100
inline uint8_t Level() {
    return LevelSlot().load(std::memory_order_relaxed);
}

/// 电平是否还新鲜（音频停下后要能自动闭嘴）
inline bool Fresh(uint32_t now_ms, uint32_t max_age_ms) {
    const uint32_t stamp = StampSlot().load(std::memory_order_relaxed);
    return stamp != 0 && static_cast<uint32_t>(now_ms - stamp) <= max_age_ms;
}

inline void Clear() {
    LevelSlot().store(0, std::memory_order_relaxed);
    StampSlot().store(0, std::memory_order_relaxed);
}

/**
 * 用一块 16bit 单声道 PCM 计算响度并发布。
 * RMS 大约：静音 <50，正常说话 300~2500，很大声 >4000。
 */
inline void Publish(const int16_t* pcm, size_t count) {
    if (pcm == nullptr || count == 0) {
        Clear();
        return;
    }
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        const int32_t s = pcm[i];
        sum += static_cast<uint64_t>(static_cast<int64_t>(s) * s);
    }
    const double rms = std::sqrt(static_cast<double>(sum) / static_cast<double>(count));
    double v = rms / 2500.0 * 100.0;
    if (v < 0.0) {
        v = 0.0;
    }
    if (v > 100.0) {
        v = 100.0;
    }
    LevelSlot().store(static_cast<uint8_t>(v + 0.5), std::memory_order_relaxed);
    StampSlot().store(static_cast<uint32_t>(esp_timer_get_time() / 1000),
                      std::memory_order_relaxed);
}

}  // namespace LipSync
