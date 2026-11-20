#include "kid_platform.h"

#include <ctime>
#include <sys/time.h>

#include <esp_log.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <ssid_manager.h>
#include <wifi_manager.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "settings.h"
#include "kid/esp_sounds.h"
#include "parent_portal.h"

#define TAG "KidPlatform"

namespace {

// Anything before 2025-01-01 means the RTC has not been set since power-up.
constexpr time_t kMinValidTime = 1735689600;
// Kid data lives in its own NVS partition so it can never fill up the main
// NVS (device identity, Wi-Fi); see partitions/v2/16m_mega3d.csv.
constexpr const char* kNvsPartition = "kidnvs";
constexpr const char* kNvsNamespace = "kidapp";
constexpr const char* kWeatherKey = "weather";
constexpr const char* kLatKey = "lat";
constexpr const char* kLonKey = "lon";

bool ChatReadyState(DeviceState s) {
    switch (s) {
        case kDeviceStateIdle:
        case kDeviceStateConnecting:
        case kDeviceStateListening:
        case kDeviceStateSpeaking:
        case kDeviceStateNotifying:
            return true;
        default:
            return false;
    }
}

}  // namespace

KidPlatform::KidPlatform() = default;
KidPlatform::~KidPlatform() = default;

int64_t KidPlatform::NowLocal() { return static_cast<int64_t>(time(nullptr)) + kUtcOffsetSeconds; }

bool KidPlatform::ClockValid() { return time(nullptr) >= kMinValidTime; }

void KidPlatform::SetClock(int64_t local_seconds) {
    struct timeval tv = {};
    tv.tv_sec = static_cast<time_t>(local_seconds - kUtcOffsetSeconds);
    settimeofday(&tv, nullptr);
    ESP_LOGI(TAG, "Clock set by hand");
}

std::string KidPlatform::WifiName() {
    auto& wifi = WifiManager::GetInstance();
    if (wifi.IsConnected()) return wifi.GetSsid();
    const auto& saved = SsidManager::GetInstance().GetSsidList();
    return saved.empty() ? "" : saved.front().ssid;
}

void KidPlatform::OpenWifiSetup() {
    if (on_open_wifi_setup) on_open_wifi_setup();
}

int KidPlatform::GetVolume() {
    auto* codec = Board::GetInstance().GetAudioCodec();
    return codec ? codec->output_volume() : 0;
}

void KidPlatform::SetVolume(int volume) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec) codec->SetOutputVolume(volume);
}

int KidPlatform::GetBrightness() {
    auto* backlight = Board::GetInstance().GetBacklight();
    return backlight ? backlight->brightness() : 0;
}

void KidPlatform::SetBrightness(int brightness) {
    auto* backlight = Board::GetInstance().GetBacklight();
    if (backlight) backlight->SetBrightness(static_cast<uint8_t>(brightness), true);
}

bool KidPlatform::IsOnline() {
    // "Online" for the kid UI means the chatbot is usable: Wi-Fi connected and
    // the xiaozhi server handshake done.
    return WifiManager::GetInstance().IsConnected() && ChatReadyState(Application::GetInstance().GetDeviceState());
}

int KidPlatform::BatteryPercent() {
    int level = -1;
    bool charging = false, discharging = false;
    if (!Board::GetInstance().GetBatteryLevel(level, charging, discharging)) return -1;
    return level;
}

bool KidPlatform::IsCharging() {
    int level = 0;
    bool charging = false, discharging = false;
    Board::GetInstance().GetBatteryLevel(level, charging, discharging);
    return charging;
}

void KidPlatform::PlaySound(kid::Sound sound) {
    Application::GetInstance().PlaySound(kid::SoundOgg(sound));
}

void KidPlatform::StartChat() {
    if (on_start_chat) on_start_chat();
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateIdle) app.ToggleChatState();
}

void KidPlatform::Interrupt() {
    // Alarms beat chat: end the conversation, then show the kid UI.
    auto& app = Application::GetInstance();
    const DeviceState s = app.GetDeviceState();
    if (s == kDeviceStateConnecting || s == kDeviceStateListening || s == kDeviceStateSpeaking) {
        app.ToggleChatState();
    }
    if (on_interrupt) on_interrupt();
}

void KidPlatform::Initialize() {
    esp_err_t err = nvs_flash_init_partition(kNvsPartition);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        // First boot after the partition was added (old assets bytes there) or
        // a full/old-format store. Only the kid partition is erased here, never
        // the main "nvs" partition.
        ESP_LOGW(TAG, "Formatting %s (%s)", kNvsPartition, esp_err_to_name(err));
        nvs_flash_erase_partition(kNvsPartition);
        err = nvs_flash_init_partition(kNvsPartition);
    }
    storage_ok_ = err == ESP_OK;
    if (!storage_ok_) ESP_LOGE(TAG, "%s init failed: %s", kNvsPartition, esp_err_to_name(err));

    // Location: set on the parent page (kidnvs), else the one the stock
    // firmware saved (read only). Values are never logged.
    if (!LoadBlob(kLatKey, latitude_) || !LoadBlob(kLonKey, longitude_)) {
        Settings location("weather", false);
        latitude_ = location.GetString("latitude");
        longitude_ = location.GetString("longitude");
    }
    std::string cached;
    if (LoadBlob(kWeatherKey, cached)) {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        kid::DecodeWeatherCache(cached, weather_);
    }
    ESP_LOGI(TAG, "Weather location %s, cache %s", HasWeatherLocation() ? "set" : "missing",
             weather_.valid ? "loaded" : "empty");
    xTaskCreate(WeatherTask, "kid_weather", 8192, this, 2, nullptr);
}

bool KidPlatform::LoadBlob(const char* key, std::string& out) {
    out.clear();
    if (!storage_ok_) return false;
    nvs_handle_t h;
    if (nvs_open_from_partition(kNvsPartition, kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    bool ok = nvs_get_blob(h, key, nullptr, &len) == ESP_OK;
    if (ok) {
        out.resize(len);
        ok = nvs_get_blob(h, key, out.data(), &len) == ESP_OK;
        if (!ok) out.clear();
    }
    nvs_close(h);
    return ok;
}

bool KidPlatform::SaveBlob(const char* key, const std::string& data) {
    if (!storage_ok_) return false;
    nvs_handle_t h;
    if (nvs_open_from_partition(kNvsPartition, kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, key, data.data(), data.size()) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool KidPlatform::GetWeather(kid::Weather& out) {
    std::lock_guard<std::mutex> lock(weather_mutex_);
    out = weather_;
    return weather_.valid;
}

bool KidPlatform::HasWeatherLocation() {
    std::lock_guard<std::mutex> lock(weather_mutex_);
    return !kid::OpenMeteoUrl(latitude_, longitude_).empty();
}

void KidPlatform::GetWeatherLocation(std::string& latitude, std::string& longitude) {
    std::lock_guard<std::mutex> lock(weather_mutex_);
    latitude = latitude_;
    longitude = longitude_;
}

bool KidPlatform::SetWeatherLocation(const std::string& latitude, const std::string& longitude) {
    if (kid::OpenMeteoUrl(latitude, longitude).empty()) return false;
    if (!SaveBlob(kLatKey, latitude) || !SaveBlob(kLonKey, longitude)) return false;
    std::lock_guard<std::mutex> lock(weather_mutex_);
    latitude_ = latitude;
    longitude_ = longitude;
    weather_.fetched_at = 0;  // refresh at the next chance
    return true;
}

bool KidPlatform::StartParentPortal(ParentPortalInfo& out) {
    if (!portal_) portal_ = std::make_unique<ParentPortal>(*this);
    parent_mode_ = true;
    if (!portal_->Start(out)) {
        parent_mode_ = false;
        return false;
    }
    return true;
}

void KidPlatform::StopParentPortal() {
    if (!portal_) return;
    portal_->Stop([this]() { parent_mode_ = false; });
}

bool KidPlatform::FetchWeather() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        url = kid::OpenMeteoUrl(latitude_, longitude_);
    }
    if (url.empty()) return false;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    http->SetTimeout(10000);
    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "Weather request failed");
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        ESP_LOGW(TAG, "Weather HTTP status %d", status ? *status : -1);
        http->Close();
        return false;
    }
    const std::string body = http->ReadAll();
    http->Close();
    kid::Weather w;
    const int64_t now = NowLocal();
    if (!kid::ParseOpenMeteo(body, now, w)) {
        ESP_LOGW(TAG, "Weather parse failed (%u bytes)", static_cast<unsigned>(body.size()));
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        weather_ = w;
    }
    SaveBlob(kWeatherKey, kid::EncodeWeatherCache(now, body));
    ESP_LOGI(TAG, "Weather updated: %s, code sky %d", kid::TempText(w.temp_now).c_str(), static_cast<int>(w.sky_now));
    return true;
}

void KidPlatform::WeatherTask(void* arg) {
    auto* self = static_cast<KidPlatform*>(arg);
    int64_t last_try = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20 * 1000));
        if (!self->HasWeatherLocation() || !self->IsOnline() || !self->ClockValid()) continue;
        kid::Weather w;
        const bool have = self->GetWeather(w);
        const int64_t now = self->NowLocal();
        const bool due = !have || now - w.fetched_at >= kid::kWeatherRefreshSeconds || w.fetched_at > now;
        // After a failure, wait 5 minutes before trying again.
        if (due && now - last_try >= 300) {
            last_try = now;
            self->FetchWeather();
        }
    }
}

void KidPlatform::MaybeStartTimeSync() {
    if (sntp_started_ || !WifiManager::GetInstance().IsConnected()) return;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();
    sntp_started_ = true;
    ESP_LOGI(TAG, "SNTP started");
}
