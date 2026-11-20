// kid::Platform on the device: clock, Wi-Fi state, battery, sounds and NVS
// storage for components/kid_apps.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "kid/platform.h"

class ParentPortal;

class KidPlatform : public kid::Platform {
public:
    KidPlatform();
    ~KidPlatform() override;

public:
    // Vietnam: UTC+7, no DST.
    static constexpr int kUtcOffsetSeconds = 7 * 3600;

    int64_t NowLocal() override;
    bool ClockValid() override;
    void SetClock(int64_t local_seconds) override;
    bool IsOnline() override;
    std::string WifiName() override;
    void OpenWifiSetup() override;
    int GetVolume() override;
    void SetVolume(int volume) override;
    int GetBrightness() override;
    void SetBrightness(int brightness) override;
    int BatteryPercent() override;
    bool IsCharging() override;
    void PlaySound(kid::Sound sound) override;
    void StartChat() override;
    void Interrupt() override;
    bool GetWeather(kid::Weather& out) override;
    bool HasWeatherLocation() override;
    void GetWeatherLocation(std::string& latitude, std::string& longitude) override;
    bool SetWeatherLocation(const std::string& latitude, const std::string& longitude) override;
    bool StartParentPortal(ParentPortalInfo& out) override;
    void StopParentPortal() override;
    // True from StartParentPortal until home Wi-Fi is back.
    bool ParentModeActive() const { return parent_mode_; }
    bool LoadBlob(const char* key, std::string& out) override;
    bool SaveBlob(const char* key, const std::string& data) override;

    // Set by the display: shows the chat screen / brings the kid UI back.
    std::function<void()> on_start_chat;
    std::function<void()> on_interrupt;
    // Set by the board: opens the Wi-Fi setup hotspot.
    std::function<void()> on_open_wifi_setup;

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

    std::unique_ptr<class ParentPortal> portal_;
    volatile bool parent_mode_ = false;

    static void WeatherTask(void* arg);
    bool FetchWeather();
};
