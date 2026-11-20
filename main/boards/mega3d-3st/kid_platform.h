// kid::Platform on the device: clock, Wi-Fi state, battery, sounds and NVS
// storage for components/kid_apps.
#pragma once

#include <functional>
#include <mutex>
#include <string>

#include "kid/platform.h"

class KidPlatform : public kid::Platform {
public:
    // Vietnam: UTC+7, no DST.
    static constexpr int kUtcOffsetSeconds = 7 * 3600;

    int64_t NowLocal() override;
    bool ClockValid() override;
    bool IsOnline() override;
    int BatteryPercent() override;
    bool IsCharging() override;
    void PlaySound(kid::Sound sound) override;
    void StartChat() override;
    bool GetWeather(kid::Weather& out) override;
    bool HasWeatherLocation() override;
    bool LoadBlob(const char* key, std::string& out) override;
    bool SaveBlob(const char* key, const std::string& data) override;

    // Set by the display: shows the chat screen.
    std::function<void()> on_start_chat;

    // Opens the kid NVS partition and starts the weather task. Call once,
    // after NVS flash init (board constructor).
    void Initialize();

    // Starts SNTP once the device is online (called periodically).
    void MaybeStartTimeSync();

private:
    bool sntp_started_ = false;
    bool storage_ok_ = false;
    std::mutex weather_mutex_;
    kid::Weather weather_;
    std::string latitude_, longitude_;

    static void WeatherTask(void* arg);
    bool FetchWeather();
};
