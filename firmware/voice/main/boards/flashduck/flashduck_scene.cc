#include "flashduck_scene.h"

#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cstdio>
#include <string>

#include "application.h"
#include "board.h"
#include "config.h"
#include "settings.h"

#define TAG "DuckScene"

namespace {

// 和服务端 panel.py 的 DEVICE_TOKEN 对应
constexpr const char* kSceneToken = "duck-cmd-2026";
constexpr int kIdleIntervalSec = 600;   // 待机：10 分钟
constexpr int kBusyIntervalSec = 20;    // 对话中：20 秒
constexpr int kHttpTimeoutMs = 20000;
constexpr const char* kPrompt = "一句话说画面里有什么，15 字以内，直接说，不要客套";

int64_t last_capture_us_ = 0;
bool first_done_ = false;

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

/// 设备是否正在被使用（非待机）。
bool Busy() {
    return Application::GetInstance().GetDeviceState() != kDeviceStateIdle;
}

void PostScene(const std::string& origin, const std::string& text) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "token", kSceneToken);
    cJSON_AddStringToObject(root, "text", text.c_str());
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        return;
    }

    esp_http_client_config_t cfg = {};
    cfg.url = (origin + "/admin/api/scene").c_str();
    cfg.timeout_ms = kHttpTimeoutMs;
    cfg.method = HTTP_METHOD_POST;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client != nullptr) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, static_cast<int>(std::string(body).size()));
        esp_http_client_perform(client);
        esp_http_client_cleanup(client);
    }
    cJSON_free(body);
}

void CaptureOnce() {
    auto* camera = Board::GetInstance().GetCamera();
    if (camera == nullptr) {
        return;
    }
    const std::string origin = OriginFromOtaUrl();
    if (origin.empty()) {
        return;
    }

    if (!camera->Capture()) {
        ESP_LOGW(TAG, "拍照失败");
        return;
    }
    auto result = camera->Explain(kPrompt);
    if (!result) {
        ESP_LOGW(TAG, "识别失败: %s", result.error().c_str());
        return;
    }
    ESP_LOGI(TAG, "画面: %s", result->c_str());
    PostScene(origin, *result);
}

}  // namespace

void FlashduckSceneTick() {
    const int64_t now = esp_timer_get_time();
    const int interval_us = (Busy() ? kBusyIntervalSec : kIdleIntervalSec) * 1000000LL;

    // 启动后先等 30 秒再拍第一张，避开开机那阵子
    if (!first_done_) {
        if (last_capture_us_ == 0) {
            last_capture_us_ = now;
            return;
        }
        if (now - last_capture_us_ < 30LL * 1000000LL) {
            return;
        }
        first_done_ = true;
    } else if (now - last_capture_us_ < interval_us) {
        return;
    }

    last_capture_us_ = now;
    CaptureOnce();
}

std::string FlashduckSceneRecent(int limit) {
    const std::string origin = OriginFromOtaUrl();
    if (origin.empty()) {
        return "";
    }
    char url[256];
    std::snprintf(url, sizeof(url), "%s/admin/api/scene?token=%s&limit=%d", origin.c_str(),
                  kSceneToken, limit);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 8000;
    cfg.method = HTTP_METHOD_GET;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return "";
    }
    std::string body;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        char buf[256];
        int total = 0;
        while (total < 4096) {
            int n = esp_http_client_read(client, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            body.append(buf, n);
            total += n;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return body;
}
