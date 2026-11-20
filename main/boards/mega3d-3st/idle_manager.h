// Idle handling for the MEGA3D 3ST (replaces PowerSaveTimer for this board):
//   1 min idle  -> dim the screen
//   2 min idle  -> screen off (a tap or any button wakes it; that first tap
//                  only wakes, it does not press whatever is under it)
//   10 min idle -> deep sleep, only on battery and when nothing is going on;
//                  a timer wakes the device shortly before the next alarm,
//                  the power button wakes it any time.
#pragma once

#include <esp_timer.h>

#include <atomic>
#include <functional>

class IdleManager {
public:
    static constexpr int kDimSeconds = 60;
    static constexpr int kScreenOffSeconds = 120;
    static constexpr int kDeepSleepSeconds = 600;

    struct Hooks {
        std::function<void(int)> set_brightness;       // 0..100, not saved
        std::function<void()> restore_brightness;      // the user's level
        std::function<void(bool)> screen_off_changed;  // show/hide the wake shield
        std::function<bool()> busy;                    // chat, alarm, parent mode...
        std::function<bool()> may_deep_sleep;          // battery, no USB host...
        std::function<void()> enter_deep_sleep;        // does not return
    };

    void Start(Hooks hooks);
    // Something happened (touch, button, chat...). Returns true if the screen
    // was off, so the caller can treat the event as "wake up" only.
    bool Activity();
    bool screen_off() const { return stage_.load() == kOff; }

private:
    enum Stage { kActive, kDim, kOff };
    Hooks hooks_;
    esp_timer_handle_t timer_ = nullptr;
    std::atomic<int> idle_s_{0};
    std::atomic<int> stage_{kActive};

    static void OnTimer(void* arg);
    void Check();
};
