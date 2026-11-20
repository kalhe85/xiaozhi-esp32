#include "parent_portal.h"

#include <cstring>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ssid_manager.h>
#include <wifi_manager.h>

#define TAG "ParentPortal"

namespace {

constexpr const char* kApPasswordKey = "ap_pass";
constexpr size_t kMaxBody = kid::kMaxRequestBody;

void LvglLock() { lvgl_port_lock(0); }
void LvglUnlock() { lvgl_port_unlock(); }

const char* StatusLine(int status) {
    switch (status) {
        case 200: return "200 OK";
        case 400: return "400 Bad Request";
        case 401: return "401 Unauthorized";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 413: return "413 Payload Too Large";
        case 429: return "429 Too Many Requests";
        default: return "500 Internal Server Error";
    }
}

}  // namespace

ParentPortal::ParentPortal(kid::Platform& platform)
    : platform_(platform), store_(platform, LvglLock, LvglUnlock), api_(store_) {}

std::string ParentPortal::Password() {
    std::string pass;
    if (platform_.LoadBlob(kApPasswordKey, pass) && pass.size() >= 8) return pass;
    // 8 random digits, easy to type on a phone; kept so phones remember it.
    pass.clear();
    for (int i = 0; i < 8; ++i) pass += static_cast<char>('0' + esp_random() % 10);
    platform_.SaveBlob(kApPasswordKey, pass);
    return pass;
}

bool ParentPortal::Start(kid::Platform::ParentPortalInfo& out) {
    if (active_) return false;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "Choco-%02X%02X", mac[4], mac[5]);
    const std::string password = Password();

    // Home Wi-Fi pauses while the hotspot runs (the board will not restart it).
    WifiManager::GetInstance().StopStation();

    ap_netif_ = esp_netif_create_default_wifi_ap();
    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.ap.ssid), ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = static_cast<uint8_t>(std::strlen(ssid));
    std::strncpy(reinterpret_cast<char*>(cfg.ap.password), password.c_str(), sizeof(cfg.ap.password));
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.max_connection = 2;
    cfg.ap.channel = 6;
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &cfg) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "Hotspot failed to start");
        esp_wifi_stop();
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
        WifiManager::GetInstance().StartStation();
        return false;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.uri_match_fn = httpd_uri_match_wildcard;
    hc.max_uri_handlers = 2;
    hc.stack_size = 8192;
    if (httpd_start(&server_, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        server_ = nullptr;
    } else {
        for (httpd_method_t m : {HTTP_GET, HTTP_POST}) {
            httpd_uri_t uri = {};
            uri.uri = "/*";
            uri.method = m;
            uri.handler = HandleRequest;
            uri.user_ctx = this;
            httpd_register_uri_handler(server_, &uri);
        }
    }
    active_ = true;
    out.ssid = ssid;
    out.password = password;
    out.url = "http://192.168.4.1";
    ESP_LOGI(TAG, "Parent mode on (hotspot %s)", ssid);
    return true;
}

void ParentPortal::Stop(std::function<void()> on_stopped) {
    if (!active_) return;
    active_ = false;
    // httpd_stop() waits for running handlers, which may be waiting for the
    // LVGL lock held by our caller: finish the teardown in its own task.
    struct Job {
        ParentPortal* self;
        std::function<void()> done;
    };
    auto* job = new Job{this, std::move(on_stopped)};
    xTaskCreate(
        [](void* arg) {
            std::unique_ptr<Job> j(static_cast<Job*>(arg));
            ParentPortal* self = j->self;
            if (self->server_) {
                httpd_stop(self->server_);
                self->server_ = nullptr;
            }
            esp_wifi_stop();
            if (self->ap_netif_) {
                esp_netif_destroy_default_wifi(self->ap_netif_);
                self->ap_netif_ = nullptr;
            }
            ESP_LOGI(TAG, "Parent mode off, back to home Wi-Fi");
            if (j->done) j->done();
            if (!SsidManager::GetInstance().GetSsidList().empty()) WifiManager::GetInstance().StartStation();
            vTaskDelete(nullptr);
        },
        "parent_stop", 4096, job, 3, nullptr);
}

esp_err_t ParentPortal::HandleRequest(httpd_req_t* req) {
    auto* self = static_cast<ParentPortal*>(req->user_ctx);
    std::string body;
    if (req->content_len > kMaxBody) {
        body.assign(kMaxBody + 1, ' ');  // let ParentApi answer 413
    } else if (req->content_len > 0) {
        body.resize(req->content_len);
        size_t got = 0;
        while (got < req->content_len) {
            const int n = httpd_req_recv(req, body.data() + got, req->content_len - got);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (n <= 0) return ESP_FAIL;
            got += static_cast<size_t>(n);
        }
    }
    char pin[16] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Pin", pin, sizeof(pin)) != ESP_OK) pin[0] = '\0';
    std::string path = req->uri;
    path = path.substr(0, path.find('?'));
    const char* method = req->method == HTTP_POST ? "POST" : "GET";

    const kid::HttpResponse r = self->api_.Handle(method, path, pin, body, esp_timer_get_time() / 1000000);
    httpd_resp_set_status(req, StatusLine(r.status));
    httpd_resp_set_type(req, r.content_type.c_str());
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, r.body.data(), static_cast<ssize_t>(r.body.size()));
}
