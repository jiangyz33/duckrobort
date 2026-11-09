#include "flashduck_remote_cmd.h"

#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string>
#include <vector>

#include "config.h"
#include "flashduck_controller.h"
#include "settings.h"

#define TAG "DuckRemoteCmd"

namespace {

// 和服务端 panel.py 里的 DEVICE_TOKEN 对应
constexpr const char* kCmdToken = "duck-cmd-2026";
constexpr int kPollIntervalMs = 15000;
constexpr int kHttpTimeoutMs = 6000;
constexpr int kMaxResponseBytes = 2048;

/// 从 OTA 地址推出服务器根地址（去掉路径部分）。
std::string OriginFromOtaUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return "";
    }
    const auto path_start = url.find('/', scheme_end + 3);
    return path_start == std::string::npos ? url : url.substr(0, path_start);
}

bool HttpGet(const std::string& url, std::string& out) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = kHttpTimeoutMs;
    cfg.method = HTTP_METHOD_GET;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    out.clear();
    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        char buf[256];
        int total = 0;
        while (total < kMaxResponseBytes) {
            int n = esp_http_client_read(client, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            out.append(buf, n);
            total += n;
        }
        ok = esp_http_client_get_status_code(client) == 200;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

void HttpPostJson(const std::string& url, const std::string& body) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = kHttpTimeoutMs;
    cfg.method = HTTP_METHOD_POST;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.size()));
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);
}

/// 取一条指令并执行，成功则回执。
bool RunOneCommand(const std::string& origin, const char* action, const char* id) {
    if (action == nullptr || action[0] == '\0') {
        return false;
    }
    const bool done = FlashduckRunAction(action);
    ESP_LOGI(TAG, "远程指令 %s -> %s", action, done ? "已执行" : "失败/队列满");
    if (done && id != nullptr && id[0] != '\0') {
        std::string body = std::string("{\"token\":\"") + kCmdToken + "\",\"id\":\"" + id + "\"}";
        HttpPostJson(origin + "/admin/api/cmd/ack", body);
    }
    return done;
}

void PollOnce() {
    const std::string origin = OriginFromOtaUrl();
    if (origin.empty()) {
        ESP_LOGW(TAG, "OTA 地址解析失败，跳过轮询");
        return;
    }
    const std::string url = origin + "/admin/api/cmd/pending?token=" + kCmdToken;
    std::string body;
    if (!HttpGet(url, body)) {
        ESP_LOGW(TAG, "取指令失败: %s", url.c_str());
        return;
    }
    if (body.empty()) {
        ESP_LOGI(TAG, "响应为空");
        return;
    }
    ESP_LOGI(TAG, "收到 %d 字节: %.100s", static_cast<int>(body.size()), body.c_str());

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "返回不是合法 JSON");
        return;
    }
    const cJSON* cmds = cJSON_GetObjectItem(root, "commands");
    if (cJSON_IsArray(cmds)) {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, cmds) {
            const cJSON* act = cJSON_GetObjectItem(item, "action");
            const cJSON* cid = cJSON_GetObjectItem(item, "id");
            RunOneCommand(origin,
                          cJSON_IsString(act) ? act->valuestring : nullptr,
                          cJSON_IsString(cid) ? cid->valuestring : nullptr);
        }
    }
    cJSON_Delete(root);
}

void PollerTask(void* arg) {
    (void)arg;
    // 开机先等网络和舵机就绪
    vTaskDelay(pdMS_TO_TICKS(8000));
    while (true) {
        PollOnce();
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
}

}  // namespace

void StartFlashduckRemoteCmdPoller() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    if (xTaskCreate(PollerTask, "duck_remote_cmd", 4096, nullptr, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "远程指令任务创建失败");
    } else {
        ESP_LOGI(TAG, "远程指令轮询已启动（每 %d 秒）", kPollIntervalMs / 1000);
    }
}
