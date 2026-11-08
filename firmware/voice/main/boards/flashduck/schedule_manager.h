#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

/**
 * 定时提醒 + 待办事项
 *
 * 都存 NVS，掉电不丢。到点且设备空闲时，主动连服务器说一句话，
 * 由服务端的大模型用人设语气复述（复用"唤醒词文本"通道）。
 *
 * 两类条目共用一个任务和一份存储：
 *   - 定时提醒（kReminder）：固定时刻重复，repeat=0 单次 / 1 每天 / 2 工作日
 *   - 待办事项（kTodo）：一次性，可带截止日期+时间，也可只记事（date=0）
 *     到点提醒后不会自动删除，要等用户说"做完了"才标记完成
 *
 * 时间来源：开机时从服务端 OTA 响应的 server_time 同步（东八区）。
 * 没同步到时间时不会触发，避免乱报。
 */
class ScheduleManager {
public:
    static ScheduleManager& GetInstance();

    /// 加载 NVS 并启动轮询任务；重复调用无副作用。
    void Init();

    /// 注册 MCP 工具（self.schedule.* / self.todo.*）
    void RegisterMcpTools();

    // ---- 定时提醒 ----
    int Add(int hour, int minute, const std::string& text, int repeat);
    std::string ListText() const;

    // ---- 待办事项 ----
    /// date 形如 20261109；传 0 表示只记事、不到点提醒。
    int AddTodo(const std::string& text, int date, int hour, int minute);
    /// 未完成待办（按时间排序，已逾期的标注出来）
    std::string ListTodoText() const;

    // ---- 倒计时 / 番茄钟 ----
    /// 从现在起 minutes 分钟后响一次；返回 id，-1 参数非法，-3 未对时
    int AddTimer(int minutes, const std::string& label);
    /// 取消所有未触发的倒计时，返回取消数量
    int CancelTimers();

    // ---- 每日晨报 ----
    /// 设置每天 hour:minute 的晨报；返回 id
    int SetBriefing(int hour, int minute);
    /// 关闭晨报，返回 1 表示确实存在并已关闭
    int ClearBriefing();
    bool HasBriefing() const;

    // ---- 通用 ----
    bool Remove(int id);
    bool MarkDone(int id);
    int Clear();

    // ---- 时间辅助（供工具层换算"明天/后天"） ----
    /// 返回本地日期 YYYYMMDD；时间未同步（年份 < 2024）返回 0
    static int TodayLocal();
    /// 取本地日期 + 时分；时间未同步返回 false
    static bool NowLocal(int* ymd, int* hour, int* minute);
    /// 日期加减天数，结果仍为 YYYYMMDD
    static int ShiftDate(int base_ymd, int day_offset);

private:
    enum Kind : uint8_t {
        kReminder = 0,
        kTodo = 1,
        kTimer = 2,     // 倒计时：一次性，到点提醒后自动删除
        kBriefing = 3,  // 每日晨报：每天固定时刻主动播报
    };

    struct Item {
        int id = 0;
        uint8_t kind = kReminder;
        int hour = 0;
        int minute = 0;
        std::string text;
        int repeat = 1;      // kReminder：0=单次 1=每天 2=工作日
        int date = 0;        // kTodo：YYYYMMDD，0=无截止
        bool done = false;   // kTodo
        int last_day = 0;    // YYYYMMDD，用于当天去重
    };

    ScheduleManager() = default;

    void Load();
    void Save() const;
    void Tick();
    void TickReminder(Item& item, const struct tm& now, int today, bool* changed,
                      std::vector<int>* to_remove);
    void TickTodo(Item& item, const struct tm& now, int today, bool* changed);
    void TickTimer(Item& item, const struct tm& now, int today, bool* changed,
                   std::vector<int>* to_remove);
    void TickBriefing(Item& item, const struct tm& now, int today, bool* changed);
    static void TaskEntry(void* arg);

    std::vector<Item> items_;
    int next_id_ = 1;
    bool started_ = false;
};
