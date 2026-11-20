#include "idle_manager.h"

#include <esp_log.h>

#define TAG "IdleManager"

void IdleManager::Start(Hooks hooks) {
    hooks_ = std::move(hooks);
    esp_timer_create_args_t args = {};
    args.callback = OnTimer;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "idle_mgr";
    args.skip_unhandled_events = true;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_, 1000 * 1000));
}

bool IdleManager::Activity() {
    idle_s_ = 0;
    const int previous = stage_.exchange(kActive);
    if (previous == kActive) return false;
    hooks_.restore_brightness();
    if (previous == kOff) {
        hooks_.screen_off_changed(false);
        ESP_LOGI(TAG, "Screen on");
    }
    return previous == kOff;
}

void IdleManager::OnTimer(void* arg) { static_cast<IdleManager*>(arg)->Check(); }

void IdleManager::Check() {
    if (hooks_.busy()) {
        Activity();
        return;
    }
    const int idle = ++idle_s_;
    if (idle >= kDimSeconds && stage_ == kActive) {
        stage_ = kDim;
        hooks_.set_brightness(10);
    }
    if (idle >= kScreenOffSeconds && stage_ == kDim) {
        stage_ = kOff;
        hooks_.set_brightness(0);
        hooks_.screen_off_changed(true);
        ESP_LOGI(TAG, "Screen off");
    }
    if (idle >= kDeepSleepSeconds && idle % 30 == 0 && hooks_.may_deep_sleep()) {
        ESP_LOGI(TAG, "Idle %d s on battery: deep sleep", idle);
        hooks_.enter_deep_sleep();
    }
}
