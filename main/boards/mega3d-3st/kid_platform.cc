#include "kid_platform.h"

#include <ctime>

#include <esp_log.h>
#include <esp_sntp.h>
#include <wifi_manager.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "settings.h"

#define TAG "KidPlatform"

namespace {

// Anything before 2025-01-01 means the RTC has not been set since power-up.
constexpr time_t kMinValidTime = 1735689600;
constexpr const char* kNvsNamespace = "kidapp";

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

int64_t KidPlatform::NowLocal() { return static_cast<int64_t>(time(nullptr)) + kUtcOffsetSeconds; }

bool KidPlatform::ClockValid() { return time(nullptr) >= kMinValidTime; }

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
    auto& app = Application::GetInstance();
    switch (sound) {
        case kid::Sound::kTap:
        case kid::Sound::kBack:
            app.PlaySound(Lang::Sounds::OGG_POPUP);
            break;
        case kid::Sound::kWin:
            app.PlaySound(Lang::Sounds::OGG_SUCCESS);
            break;
        case kid::Sound::kLose:
        case kid::Sound::kAlarm:
        case kid::Sound::kReminder:
            app.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
            break;
        default:
            // Simon notes come with the game assets (Phase 3).
            app.PlaySound(Lang::Sounds::OGG_POPUP);
            break;
    }
}

void KidPlatform::StartChat() {
    if (on_start_chat) on_start_chat();
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateIdle) app.ToggleChatState();
}

bool KidPlatform::LoadBlob(const char* key, std::string& out) {
    Settings settings(kNvsNamespace, false);
    out = settings.GetString(key);
    return !out.empty();
}

bool KidPlatform::SaveBlob(const char* key, const std::string& data) {
    Settings settings(kNvsNamespace, true);
    settings.SetString(key, data);
    return true;
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
