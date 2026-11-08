#include "schedule_manager.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cJSON.h>
#include <algorithm>
#include <cstdio>
#include <ctime>

#include "application.h"
#include "mcp_server.h"
#include "settings.h"

#define TAG "ScheduleMgr"

namespace {
constexpr const char* kNs = "sched";
constexpr const char* kKey = "items";
constexpr int kPollMs = 15000;
constexpr int kReminderWindowMin = 5;   // 定时提醒：到点后 5 分钟内都算数
constexpr int kTodoWindowMin = 15;      // 待办：到点后 15 分钟内都算数
constexpr int kMaxItems = 32;

int StampYmd(const struct tm& t) {
    return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

/// 把 20261109 转成"11月9日"
std::string FormatYmd(int ymd) {
    if (ymd <= 0) {
        return "不限日期";
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d月%d日", (ymd / 100) % 100, ymd % 100);
    return buf;
}

/// 相对今天的说法：今天/明天/后天/具体日期
std::string RelativeDay(int date, int today) {
    if (date <= 0) {
        return "不限日期";
    }
    if (today > 0) {
        if (date == today) {
            return "今天";
        }
        if (date == today + 1 || date == ScheduleManager::ShiftDate(today, 1)) {
            return "明天";
        }
        if (date == ScheduleManager::ShiftDate(today, 2)) {
            return "后天";
        }
    }
    return FormatYmd(date);
}
}  // namespace

ScheduleManager& ScheduleManager::GetInstance() {
    static ScheduleManager instance;
    return instance;
}

// ---------------------------------------------------------------- 时间辅助
int ScheduleManager::TodayLocal() {
    const time_t now = time(nullptr);
    struct tm t {};
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < 2024) {
        return 0;
    }
    return StampYmd(t);
}

bool ScheduleManager::NowLocal(int* ymd, int* hour, int* minute) {
    const time_t now = time(nullptr);
    struct tm t {};
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < 2024) {
        return false;
    }
    if (ymd != nullptr) {
        *ymd = StampYmd(t);
    }
    if (hour != nullptr) {
        *hour = t.tm_hour;
    }
    if (minute != nullptr) {
        *minute = t.tm_min;
    }
    return true;
}

int ScheduleManager::ShiftDate(int base_ymd, int day_offset) {
    if (base_ymd <= 0) {
        return 0;
    }
    struct tm t {};
    t.tm_year = base_ymd / 10000 - 1900;
    t.tm_mon = (base_ymd / 100) % 100 - 1;
    t.tm_mday = base_ymd % 100;
    t.tm_hour = 12;  // 取正午，避开夏令时边界
    t.tm_isdst = -1;
    t.tm_mday += day_offset;
    if (mktime(&t) == static_cast<time_t>(-1)) {
        return base_ymd;
    }
    return StampYmd(t);
}

// ---------------------------------------------------------------- 持久化
void ScheduleManager::Load() {
    items_.clear();
    next_id_ = 1;
    Settings settings(kNs, false);
    std::string raw = settings.GetString(kKey, "");
    if (raw.empty()) {
        return;
    }
    cJSON* root = cJSON_Parse(raw.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "数据损坏，已忽略");
        return;
    }
    cJSON* it = nullptr;
    cJSON_ArrayForEach(it, root) {
        auto id = cJSON_GetObjectItem(it, "id");
        auto txt = cJSON_GetObjectItem(it, "t");
        if (!cJSON_IsNumber(id) || !cJSON_IsString(txt)) {
            continue;
        }
        Item item;
        item.id = id->valueint;
        item.text = txt->valuestring;
        auto k = cJSON_GetObjectItem(it, "k");
        auto h = cJSON_GetObjectItem(it, "h");
        auto m = cJSON_GetObjectItem(it, "m");
        auto rep = cJSON_GetObjectItem(it, "r");
        auto day = cJSON_GetObjectItem(it, "d");
        auto date = cJSON_GetObjectItem(it, "date");
        auto done = cJSON_GetObjectItem(it, "done");
        item.kind = cJSON_IsNumber(k) ? static_cast<uint8_t>(k->valueint) : kReminder;
        item.hour = cJSON_IsNumber(h) ? h->valueint : 0;
        item.minute = cJSON_IsNumber(m) ? m->valueint : 0;
        item.repeat = cJSON_IsNumber(rep) ? rep->valueint : 1;
        item.last_day = cJSON_IsNumber(day) ? day->valueint : 0;
        item.date = cJSON_IsNumber(date) ? date->valueint : 0;
        item.done = cJSON_IsTrue(done);
        if (item.id >= next_id_) {
            next_id_ = item.id + 1;
        }
        items_.push_back(item);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "已加载 %d 条（提醒 + 待办）", static_cast<int>(items_.size()));
}

void ScheduleManager::Save() const {
    cJSON* root = cJSON_CreateArray();
    for (const auto& it : items_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", it.id);
        cJSON_AddNumberToObject(o, "k", it.kind);
        cJSON_AddNumberToObject(o, "h", it.hour);
        cJSON_AddNumberToObject(o, "m", it.minute);
        cJSON_AddStringToObject(o, "t", it.text.c_str());
        cJSON_AddNumberToObject(o, "r", it.repeat);
        cJSON_AddNumberToObject(o, "d", it.last_day);
        cJSON_AddNumberToObject(o, "date", it.date);
        cJSON_AddBoolToObject(o, "done", it.done);
        cJSON_AddItemToArray(root, o);
    }
    char* str = cJSON_PrintUnformatted(root);
    if (str != nullptr) {
        Settings settings(kNs, true);
        settings.SetString(kKey, str);
        cJSON_free(str);
    }
    cJSON_Delete(root);
}

// ---------------------------------------------------------------- 生命周期
void ScheduleManager::Init() {
    if (started_) {
        return;
    }
    started_ = true;
    Load();
    // 栈放大到 6K：注册 MCP 工具要构造一批 Property 对象，4K 偏紧
    xTaskCreate(TaskEntry, "sched_task", 6144, this, 2, nullptr);
    ESP_LOGI(TAG, "定时提醒 / 待办服务已启动");
}

void ScheduleManager::TaskEntry(void* arg) {
    auto* self = static_cast<ScheduleManager*>(arg);

    // MCP 工具延迟到 UI / 音频初始化完成后再注册。
    // 若在板级初始化阶段（LVGL 尚未初始化）注册，会踩坏 LVGL 的堆元数据，
    // 表现为 SetupUI -> ApplyPupil 里 lv_free 死循环 + 任务看门狗超时。
    vTaskDelay(pdMS_TO_TICKS(3000));
    self->RegisterMcpTools();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
        self->Tick();
    }
}

void ScheduleManager::Tick() {
    if (items_.empty()) {
        return;
    }
    const time_t now = time(nullptr);
    struct tm t {};
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < 2024) {
        return;  // 还没对表
    }
    const int today = StampYmd(t);
    bool changed = false;
    std::vector<int> to_remove;

    for (auto& item : items_) {
        switch (item.kind) {
            case kTodo:
                TickTodo(item, t, today, &changed);
                break;
            case kTimer:
                TickTimer(item, t, today, &changed, &to_remove);
                break;
            case kBriefing:
                TickBriefing(item, t, today, &changed);
                break;
            default:
                TickReminder(item, t, today, &changed, &to_remove);
                break;
        }
    }

    // 单次提醒用完即删：放到循环外做，避免遍历时删除导致迭代器失效
    if (!to_remove.empty()) {
        for (int id : to_remove) {
            items_.erase(std::remove_if(items_.begin(), items_.end(),
                                        [id](const Item& x) { return x.id == id; }),
                         items_.end());
        }
        changed = true;
    }
    if (changed) {
        Save();
    }
}

void ScheduleManager::TickReminder(Item& item, const struct tm& now, int today, bool* changed,
                                   std::vector<int>* to_remove) {
    if (item.last_day == today) {
        return;
    }
    if (now.tm_hour != item.hour) {
        return;
    }
    const int diff = now.tm_min - item.minute;
    if (diff < 0 || diff > kReminderWindowMin) {
        return;
    }
    if (item.repeat == 2 && (now.tm_wday == 0 || now.tm_wday == 6)) {
        return;  // 工作日模式跳过周末
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        return;
    }

    char hm[8];
    snprintf(hm, sizeof(hm), "%02d:%02d", item.hour, item.minute);
    std::string spoken = "【定时提醒】" + std::string(hm) + "，提醒主人：" + item.text +
                         "。请用一句话温柔地提醒他。";
    ESP_LOGI(TAG, "触发提醒 #%d %s -> %s", item.id, hm, item.text.c_str());
    if (!Application::GetInstance().SpeakProactive(spoken)) {
        return;
    }
    item.last_day = today;
    *changed = true;

    if (item.repeat == 0) {
        to_remove->push_back(item.id);
    }
}

void ScheduleManager::TickTodo(Item& item, const struct tm& now, int today, bool* changed) {
    if (item.done || item.date <= 0) {
        return;  // 已完成，或只记事不提醒
    }
    if (item.date != today || item.last_day == today) {
        return;
    }
    if (now.tm_hour != item.hour) {
        return;
    }
    const int diff = now.tm_min - item.minute;
    if (diff < 0 || diff > kTodoWindowMin) {
        return;
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        return;
    }

    char hm[16];
    snprintf(hm, sizeof(hm), "%02d:%02d", item.hour, item.minute);
    std::string spoken = "【待办提醒】" + std::string(hm) + "，主人之前记下的待办是：" + item.text +
                         "。请用一句话提醒他，并问他要不要标记为已完成。";
    ESP_LOGI(TAG, "触发待办 #%d %s -> %s", item.id, hm, item.text.c_str());
    if (!Application::GetInstance().SpeakProactive(spoken)) {
        return;
    }
    item.last_day = today;
    *changed = true;
    // 待办不自动删除：等用户说"做完了"，由模型调用 self.todo.done
}

// ---------------------------------------------------------------- 倒计时 / 晨报
void ScheduleManager::TickTimer(Item& item, const struct tm& now, int today, bool* changed,
                                std::vector<int>* to_remove) {
    if (item.last_day == today || item.date != today) {
        return;
    }
    if (now.tm_hour != item.hour) {
        return;
    }
    const int diff = now.tm_min - item.minute;
    if (diff < 0 || diff > 2) {  // 倒计时窗口收紧到 2 分钟
        return;
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        return;
    }

    std::string spoken = "【倒计时】时间到了，主人设的「" + item.text +
                         "」到点了。请用一句话提醒他，语气轻松一点。";
    ESP_LOGI(TAG, "触发倒计时 #%d -> %s", item.id, item.text.c_str());
    if (!Application::GetInstance().SpeakProactive(spoken)) {
        return;
    }
    item.last_day = today;
    *changed = true;
    to_remove->push_back(item.id);  // 一次性
}

void ScheduleManager::TickBriefing(Item& item, const struct tm& now, int today, bool* changed) {
    if (item.last_day == today) {
        return;
    }
    if (now.tm_hour != item.hour) {
        return;
    }
    const int diff = now.tm_min - item.minute;
    if (diff < 0 || diff > kReminderWindowMin) {
        return;
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        return;
    }

    // 主动播报里让模型自己去调工具，这样天气/待办/新闻都是实时数据
    std::string spoken =
        "【早晨播报】现在是早上，请主动跟主人打个招呼并做一次今日播报："
        "先用 get_weather_now 查今天的天气，再用 self.todo.list 看看有什么待办，"
        "需要的话用 web_search 补一条新闻。三句话以内说完，语气温柔，句末带表情。";
    ESP_LOGI(TAG, "触发晨报 #%d", item.id);
    if (!Application::GetInstance().SpeakProactive(spoken)) {
        return;
    }
    item.last_day = today;
    *changed = true;
}

int ScheduleManager::AddTimer(int minutes, const std::string& label) {
    if (minutes <= 0 || minutes > 24 * 60) {
        return -1;
    }
    if (static_cast<int>(items_.size()) >= kMaxItems) {
        return -2;
    }
    int ymd = 0;
    int hour = 0;
    int minute = 0;
    if (!NowLocal(&ymd, &hour, &minute)) {
        return -3;
    }

    int total = hour * 60 + minute + minutes;
    const int day_add = total / (24 * 60);
    total %= (24 * 60);

    Item item;
    item.id = next_id_++;
    item.kind = kTimer;
    item.date = ShiftDate(ymd, day_add);
    item.hour = total / 60;
    item.minute = total % 60;
    item.text = label.empty() ? "倒计时" : label;
    items_.push_back(item);
    Save();
    return item.id;
}

int ScheduleManager::CancelTimers() {
    const size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [](const Item& x) { return x.kind == kTimer; }),
                 items_.end());
    const int removed = static_cast<int>(before - items_.size());
    if (removed > 0) {
        Save();
    }
    return removed;
}

int ScheduleManager::SetBriefing(int hour, int minute) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return -1;
    }
    ClearBriefing();
    if (static_cast<int>(items_.size()) >= kMaxItems) {
        return -2;
    }
    Item item;
    item.id = next_id_++;
    item.kind = kBriefing;
    item.hour = hour;
    item.minute = minute;
    item.text = "每日晨报";
    items_.push_back(item);
    Save();
    return item.id;
}

int ScheduleManager::ClearBriefing() {
    const size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [](const Item& x) { return x.kind == kBriefing; }),
                 items_.end());
    if (items_.size() != before) {
        Save();
        return 1;
    }
    return 0;
}

bool ScheduleManager::HasBriefing() const {
    for (const auto& it : items_) {
        if (it.kind == kBriefing) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- 增删查
int ScheduleManager::Add(int hour, int minute, const std::string& text, int repeat) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || text.empty()) {
        return -1;
    }
    if (static_cast<int>(items_.size()) >= kMaxItems) {
        return -2;
    }
    Item item;
    item.id = next_id_++;
    item.kind = kReminder;
    item.hour = hour;
    item.minute = minute;
    item.text = text;
    item.repeat = (repeat >= 0 && repeat <= 2) ? repeat : 1;
    items_.push_back(item);
    Save();
    return item.id;
}

int ScheduleManager::AddTodo(const std::string& text, int date, int hour, int minute) {
    if (text.empty()) {
        return -1;
    }
    if (static_cast<int>(items_.size()) >= kMaxItems) {
        return -2;
    }
    Item item;
    item.id = next_id_++;
    item.kind = kTodo;
    item.text = text;
    item.date = (date > 0) ? date : 0;
    item.hour = (item.date > 0) ? hour : 0;
    item.minute = (item.date > 0) ? minute : 0;
    item.done = false;
    items_.push_back(item);
    Save();
    return item.id;
}

std::string ScheduleManager::ListText() const {
    std::string out;
    const int today = TodayLocal();
    int count = 0;
    for (const auto& it : items_) {
        if (it.kind != kReminder) {
            continue;
        }
        if (count == 0) {
            out = "当前定时提醒：";
        }
        char buf[128];
        const char* r = it.repeat == 0 ? "单次" : (it.repeat == 2 ? "工作日" : "每天");
        snprintf(buf, sizeof(buf), " 第%d条，%02d:%02d，%s（%s）", it.id, it.hour, it.minute,
                 it.text.c_str(), r);
        out += buf;
        ++count;
    }
    if (count == 0) {
        return "还没有设置定时提醒。";
    }
    (void)today;
    return out;
}

std::string ScheduleManager::ListTodoText() const {
    const int today = TodayLocal();
    std::vector<const Item*> pending;
    for (const auto& it : items_) {
        if (it.kind == kTodo && !it.done) {
            pending.push_back(&it);
        }
    }
    if (pending.empty()) {
        return "你没有未完成的待办，可以歇会儿了。";
    }
    std::sort(pending.begin(), pending.end(), [](const Item* a, const Item* b) {
        const int da = (a->date > 0) ? a->date : 99999999;
        const int db = (b->date > 0) ? b->date : 99999999;
        if (da != db) {
            return da < db;
        }
        if (a->hour != b->hour) {
            return a->hour < b->hour;
        }
        return a->minute < b->minute;
    });

    char head[64];
    snprintf(head, sizeof(head), "你有 %d 件待办：", static_cast<int>(pending.size()));
    std::string out = head;
    for (const auto* p : pending) {
        char buf[192];
        if (p->date > 0) {
            const bool overdue = (today > 0 && p->date < today);
            snprintf(buf, sizeof(buf), " 第%d条，%s %02d:%02d，%s%s。", p->id,
                     RelativeDay(p->date, today).c_str(), p->hour, p->minute, p->text.c_str(),
                     overdue ? "（已过期）" : "");
        } else {
            snprintf(buf, sizeof(buf), " 第%d条，还没定时间，%s。", p->id, p->text.c_str());
        }
        out += buf;
    }
    return out;
}

bool ScheduleManager::Remove(int id) {
    const size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [id](const Item& x) { return x.id == id; }),
                 items_.end());
    if (items_.size() != before) {
        Save();
        return true;
    }
    return false;
}

bool ScheduleManager::MarkDone(int id) {
    for (auto& it : items_) {
        if (it.id == id) {
            if (it.done) {
                return true;
            }
            it.done = true;
            Save();
            return true;
        }
    }
    return false;
}

int ScheduleManager::Clear() {
    const int n = static_cast<int>(items_.size());
    items_.clear();
    Save();
    return n;
}

// ---------------------------------------------------------------- MCP 工具
void ScheduleManager::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    // ============ 定时提醒 ============
    mcp.AddTool(
        "self.schedule.add",
        "增加一条「定时提醒」，到点会主动开口。适合「每天八点叫我吃药」「明早七点叫我」这类"
        "重复或固定时刻的提醒。time 用 24 小时制 HH:MM；repeat 0=只响一次 1=每天 2=工作日。",
        PropertyList({
            Property("time", kPropertyTypeString),
            Property("text", kPropertyTypeString),
            Property("repeat", kPropertyTypeInteger, 1),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto time = properties["time"].value<std::string>();
            auto text = properties["text"].value<std::string>();
            auto repeat = properties["repeat"].value<int>();
            int hour = 0;
            int minute = 0;
            if (sscanf(time.c_str(), "%d:%d", &hour, &minute) != 2) {
                return std::string("时间格式不对，要用 HH:MM，例如 08:30");
            }
            const int id = ScheduleManager::GetInstance().Add(hour, minute, text, repeat);
            if (id == -1) {
                return std::string("参数不合法（时间或内容为空）");
            }
            if (id == -2) {
                return std::string("条目太多了，先删掉几条吧");
            }
            const char* r = repeat == 0 ? "只响一次" : (repeat == 2 ? "工作日" : "每天");
            char buf[128];
            snprintf(buf, sizeof(buf), "好，%02d:%02d 我会提醒你（%s），编号 %d", hour, minute, r, id);
            return std::string(buf);
        });

    mcp.AddTool("self.schedule.list", "列出所有定时提醒。", PropertyList(),
                [](const PropertyList&) -> ReturnValue {
                    return ScheduleManager::GetInstance().ListText();
                });

    mcp.AddTool("self.schedule.remove", "按编号删除一条定时提醒。",
                PropertyList({Property("id", kPropertyTypeInteger)}),
                [](const PropertyList& properties) -> ReturnValue {
                    const int id = properties["id"].value<int>();
                    char buf[64];
                    if (ScheduleManager::GetInstance().Remove(id)) {
                        snprintf(buf, sizeof(buf), "已删除第 %d 条", id);
                    } else {
                        snprintf(buf, sizeof(buf), "没有编号为 %d 的条目", id);
                    }
                    return std::string(buf);
                });

    // ============ 待办事项 ============
    mcp.AddTool(
        "self.todo.add",
        "记录一条待办事项。用户说「帮我记一下」「别忘了」「记得要做某事」时调用。"
        "text 是要做的事；time 是截止时刻 HH:MM，没有就传空字符串；"
        "day_offset 表示哪天：0=今天 1=明天 2=后天 …（默认 0）。"
        "如果用户只说事情没说时间，把 time 留空、day_offset 传 -1，表示只记事不提醒。",
        PropertyList({
            Property("text", kPropertyTypeString),
            Property("time", kPropertyTypeString, std::string("")),
            Property("day_offset", kPropertyTypeInteger, 0),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto text = properties["text"].value<std::string>();
            auto time = properties["time"].value<std::string>();
            auto day_offset = properties["day_offset"].value<int>();
            if (text.empty()) {
                return std::string("要记的事情不能为空");
            }

            int today = 0;
            int now_h = 0;
            int now_m = 0;
            const bool time_ok = ScheduleManager::NowLocal(&today, &now_h, &now_m);
            if (!time_ok) {
                return std::string("设备还没对时，我先记下了，等联网后再说时间");
            }

            // 只记事、不设提醒
            if (day_offset < 0 || time.empty()) {
                const int id = ScheduleManager::GetInstance().AddTodo(text, 0, 0, 0);
                if (id == -1) {
                    return std::string("参数不合法");
                }
                if (id == -2) {
                    return std::string("条目太多了，先清理一下");
                }
                char buf[160];
                snprintf(buf, sizeof(buf), "记下了：「%s」，编号 %d。需要我定时提醒吗？",
                         text.c_str(), id);
                return std::string(buf);
            }

            int hour = 0;
            int minute = 0;
            if (sscanf(time.c_str(), "%d:%d", &hour, &minute) != 2 || hour > 23 || minute > 59) {
                return std::string("时间格式不对，要用 HH:MM，例如 15:30");
            }

            int date = ScheduleManager::ShiftDate(today, std::clamp(day_offset, 0, 30));
            bool rolled = false;
            if (date == today && (hour * 60 + minute) <= (now_h * 60 + now_m)) {
                // 今天这个点已经过了，顺延到明天，免得永不触发
                date = ScheduleManager::ShiftDate(today, 1);
                rolled = true;
            }

            const int id = ScheduleManager::GetInstance().AddTodo(text, date, hour, minute);
            if (id == -1) {
                return std::string("参数不合法");
            }
            if (id == -2) {
                return std::string("条目太多了，先清理一下");
            }
            char buf[192];
            snprintf(buf, sizeof(buf), "记下了：「%s」，%s %02d:%02d 提醒你，编号 %d%s",
                     text.c_str(), RelativeDay(date, today).c_str(), hour, minute, id,
                     rolled ? "（今天这个点已经过了，给你挪到明天了）" : "");
            return std::string(buf);
        });

    mcp.AddTool("self.todo.list",
                "列出所有未完成的待办事项。用户问「我还有什么要做的」「今天有什么安排」"
                "「还有哪些事没做」时调用。",
                PropertyList(), [](const PropertyList&) -> ReturnValue {
                    return ScheduleManager::GetInstance().ListTodoText();
                });

    mcp.AddTool("self.todo.done",
                "把某条待办标记为已完成。用户说「做完了」「那个事搞定了」「划掉第几条」时调用。",
                PropertyList({Property("id", kPropertyTypeInteger)}),
                [](const PropertyList& properties) -> ReturnValue {
                    const int id = properties["id"].value<int>();
                    char buf[64];
                    if (ScheduleManager::GetInstance().MarkDone(id)) {
                        snprintf(buf, sizeof(buf), "好，第 %d 条已标记完成", id);
                    } else {
                        snprintf(buf, sizeof(buf), "没有编号为 %d 的待办", id);
                    }
                    return std::string(buf);
                });

    mcp.AddTool("self.todo.remove", "按编号彻底删除一条待办（用于记错了要撤销）。",
                PropertyList({Property("id", kPropertyTypeInteger)}),
                [](const PropertyList& properties) -> ReturnValue {
                    const int id = properties["id"].value<int>();
                    char buf[64];
                    if (ScheduleManager::GetInstance().Remove(id)) {
                        snprintf(buf, sizeof(buf), "已删除第 %d 条待办", id);
                    } else {
                        snprintf(buf, sizeof(buf), "没有编号为 %d 的待办", id);
                    }
                    return std::string(buf);
                });

    // ============ 倒计时 / 番茄钟 ============
    mcp.AddTool(
        "self.timer.start",
        "设置倒计时或番茄钟。用户说「十分钟后提醒我」「定个25分钟的番茄钟」"
        "「专注半小时」「帮我计时」时调用。minutes 是分钟数(1~1440)，label 是这次计时的名字。",
        PropertyList({
            Property("minutes", kPropertyTypeInteger, 25),
            Property("label", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            const int minutes = properties["minutes"].value<int>();
            auto label = properties["label"].value<std::string>();
            const int id = ScheduleManager::GetInstance().AddTimer(minutes, label);
            if (id == -3) {
                return std::string("设备还没对时，等联网后再试");
            }
            if (id == -1) {
                return std::string("分钟数要在 1~1440 之间");
            }
            if (id == -2) {
                return std::string("条目太多了，先清理一下");
            }
            char buf[128];
            snprintf(buf, sizeof(buf), "好，%d 分钟后叫你（编号 %d）", minutes, id);
            return std::string(buf);
        });

    mcp.AddTool("self.timer.cancel", "取消所有还没到点的倒计时。", PropertyList(),
                [](const PropertyList&) -> ReturnValue {
                    const int n = ScheduleManager::GetInstance().CancelTimers();
                    char buf[64];
                    snprintf(buf, sizeof(buf), n > 0 ? "取消了 %d 个倒计时" : "没有正在跑的倒计时", n);
                    return std::string(buf);
                });

    // ============ 每日晨报 ============
    mcp.AddTool(
        "self.briefing.set",
        "设置每日晨报：每天固定时刻主动播报天气、待办和一条新闻。"
        "用户说「每天八点给我播个晨报」「早上叫我顺便说说天气」时调用。time 用 HH:MM。",
        PropertyList({Property("time", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            auto time = properties["time"].value<std::string>();
            int hour = 0;
            int minute = 0;
            if (sscanf(time.c_str(), "%d:%d", &hour, &minute) != 2) {
                return std::string("时间格式不对，要用 HH:MM，例如 08:00");
            }
            const int id = ScheduleManager::GetInstance().SetBriefing(hour, minute);
            if (id == -1) {
                return std::string("时间不合法");
            }
            if (id == -2) {
                return std::string("条目太多了，先清理一下");
            }
            char buf[96];
            snprintf(buf, sizeof(buf), "好，每天 %02d:%02d 给你播晨报（编号 %d）", hour, minute, id);
            return std::string(buf);
        });

    mcp.AddTool("self.briefing.off", "关闭每日晨报。", PropertyList(),
                [](const PropertyList&) -> ReturnValue {
                    return std::string(ScheduleManager::GetInstance().ClearBriefing()
                                           ? "晨报已关闭"
                                           : "本来就没开晨报");
                });

    ESP_LOGI(TAG, "MCP 工具注册完成（self.schedule.* + self.todo.*）");
}
