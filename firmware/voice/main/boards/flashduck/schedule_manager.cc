#include "schedule_manager.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cJSON.h>
#include <cstdio>
#include <ctime>

#include <algorithm>

#include "application.h"
#include "mcp_server.h"
#include "settings.h"

#define TAG "ScheduleMgr"

namespace {
constexpr const char* kNs = "sched";
constexpr const char* kKey = "items";
constexpr int kPollMs = 15000;
constexpr int kWindowMin = 5;   // 到点后 5 分钟内都算"该响了"（设备忙完能补上）

int TodayStamp(const struct tm& t) {
    return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}
}  // namespace

ScheduleManager& ScheduleManager::GetInstance() {
    static ScheduleManager instance;
    return instance;
}

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
        ESP_LOGW(TAG, "提醒数据损坏，已忽略");
        return;
    }
    cJSON* it = nullptr;
    cJSON_ArrayForEach(it, root) {
        Item item;
        auto id = cJSON_GetObjectItem(it, "id");
        auto h = cJSON_GetObjectItem(it, "h");
        auto m = cJSON_GetObjectItem(it, "m");
        auto txt = cJSON_GetObjectItem(it, "t");
        auto rep = cJSON_GetObjectItem(it, "r");
        auto day = cJSON_GetObjectItem(it, "d");
        if (!cJSON_IsNumber(id) || !cJSON_IsNumber(h) || !cJSON_IsNumber(m) ||
            !cJSON_IsString(txt)) {
            continue;
        }
        item.id = id->valueint;
        item.hour = h->valueint;
        item.minute = m->valueint;
        item.text = txt->valuestring;
        item.repeat = cJSON_IsNumber(rep) ? rep->valueint : 1;
        item.last_day = cJSON_IsNumber(day) ? day->valueint : 0;
        if (item.id >= next_id_) {
            next_id_ = item.id + 1;
        }
        items_.push_back(item);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "已加载 %d 条提醒", static_cast<int>(items_.size()));
}

void ScheduleManager::Save() const {
    cJSON* root = cJSON_CreateArray();
    for (const auto& it : items_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", it.id);
        cJSON_AddNumberToObject(o, "h", it.hour);
        cJSON_AddNumberToObject(o, "m", it.minute);
        cJSON_AddStringToObject(o, "t", it.text.c_str());
        cJSON_AddNumberToObject(o, "r", it.repeat);
        cJSON_AddNumberToObject(o, "d", it.last_day);
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

void ScheduleManager::Init() {
    if (started_) {
        return;
    }
    started_ = true;
    Load();
    xTaskCreate(TaskEntry, "sched_task", 4096, this, 2, nullptr);
    ESP_LOGI(TAG, "定时提醒服务已启动");
}

void ScheduleManager::TaskEntry(void* arg) {
    auto* self = static_cast<ScheduleManager*>(arg);
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
        // 还没和服务端对表，先不触发，避免一开机乱报
        return;
    }
    const int today = TodayStamp(t);
    bool changed = false;

    for (auto& it : items_) {
        if (!it.enabled || it.last_day == today) {
            continue;
        }
        if (t.tm_hour != it.hour) {
            continue;
        }
        const int diff = t.tm_min - it.minute;
        if (diff < 0 || diff > kWindowMin) {
            continue;
        }
        if (it.repeat == 2 && (t.tm_wday == 0 || t.tm_wday == 6)) {
            continue;  // 工作日模式跳过周末
        }
        // 设备忙就先不响，下一个 tick 再试（还在窗口内）
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            continue;
        }

        char hm[8];
        snprintf(hm, sizeof(hm), "%02d:%02d", it.hour, it.minute);
        std::string spoken = "【定时提醒】" + std::string(hm) + "，提醒主人：" + it.text +
                             "。请用一句话温柔地提醒他。";
        ESP_LOGI(TAG, "触发提醒 #%d %s -> %s", it.id, hm, it.text.c_str());
        if (!Application::GetInstance().SpeakProactive(spoken)) {
            continue;
        }
        it.last_day = today;
        changed = true;
        if (it.repeat == 0) {
            // 单次提醒用完即删
            const int id = it.id;
            items_.erase(std::remove_if(items_.begin(), items_.end(),
                                        [id](const Item& x) { return x.id == id; }),
                         items_.end());
            Save();
            return;
        }
    }
    if (changed) {
        Save();
    }
}

int ScheduleManager::Add(int hour, int minute, const std::string& text, int repeat) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || text.empty()) {
        return -1;
    }
    if (items_.size() >= 16) {
        return -2;
    }
    Item item;
    item.id = next_id_++;
    item.hour = hour;
    item.minute = minute;
    item.text = text;
    item.repeat = (repeat >= 0 && repeat <= 2) ? repeat : 1;
    items_.push_back(item);
    Save();
    return item.id;
}

std::string ScheduleManager::ListText() const {
    if (items_.empty()) {
        return "还没有设置提醒";
    }
    std::string out = "当前提醒：";
    for (const auto& it : items_) {
        char buf[64];
        const char* r = it.repeat == 0 ? "单次" : (it.repeat == 2 ? "工作日" : "每天");
        snprintf(buf, sizeof(buf), " #%d %02d:%02d %s（%s）", it.id, it.hour, it.minute,
                 it.text.c_str(), r);
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

int ScheduleManager::Clear() {
    const int n = static_cast<int>(items_.size());
    items_.clear();
    Save();
    return n;
}

// ---------------------------------------------------------------- MCP 工具
void ScheduleManager::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.schedule.add",
        "给主人的设备增加一条定时提醒。用户说“每天八点提醒我吃药”“明早七点叫我”时调用。"
        "time 用 24 小时制 HH:MM；repeat 0=只响一次 1=每天 2=工作日。",
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
                return std::string("提醒太多了（最多 16 条），先删掉几条吧");
            }
            const char* r = repeat == 0 ? "只响一次" : (repeat == 2 ? "工作日" : "每天");
            char buf[128];
            snprintf(buf, sizeof(buf), "好，%02d:%02d 我会提醒你（%s），编号 %d", hour, minute, r, id);
            return std::string(buf);
        });

    mcp.AddTool(
        "self.schedule.list",
        "列出当前所有定时提醒。",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return ScheduleManager::GetInstance().ListText();
        });

    mcp.AddTool(
        "self.schedule.remove",
        "按编号删除一条定时提醒。",
        PropertyList({
            Property("id", kPropertyTypeInteger),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            const int id = properties["id"].value<int>();
            if (ScheduleManager::GetInstance().Remove(id)) {
                char buf[64];
                snprintf(buf, sizeof(buf), "已删除第 %d 条提醒", id);
                return std::string(buf);
            }
            char buf[64];
            snprintf(buf, sizeof(buf), "没有编号为 %d 的提醒", id);
            return std::string(buf);
        });

    ESP_LOGI(TAG, "MCP 工具注册完成（self.schedule.*）");
}
