#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * 定时提醒 / 定时任务
 *
 * 存在 NVS 里，掉电不丢。到点时如果设备空闲，就主动连上服务器说一句话，
 * 由服务端的大模型用人设语气复述出来（复用"唤醒词文本"通道）。
 *
 * 时间来源：开机时从服务端 OTA 响应的 server_time 同步（东八区）。
 * 没同步到时间时不会触发，避免乱报。
 */
class ScheduleManager {
public:
    static ScheduleManager& GetInstance();

    /// 加载 NVS 并启动轮询任务；重复调用无副作用。
    void Init();

    /// 注册 MCP 工具（self.schedule.*）
    void RegisterMcpTools();

    int Add(int hour, int minute, const std::string& text, int repeat);
    std::string ListText() const;
    bool Remove(int id);
    int Clear();

private:
    struct Item {
        int id = 0;
        int hour = 0;
        int minute = 0;
        std::string text;
        int repeat = 1;       // 0=单次 1=每天 2=工作日
        bool enabled = true;
        int last_day = 0;      // YYYYMMDD，用于当天去重
    };

    ScheduleManager() = default;

    void Load();
    void Save() const;
    void Tick();
    static void TaskEntry(void* arg);

    std::vector<Item> items_;
    int next_id_ = 1;
    bool started_ = false;
};
