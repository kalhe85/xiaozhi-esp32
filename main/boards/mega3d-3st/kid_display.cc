#include "kid_display.h"

#include <esp_log.h>

#include "application.h"
#include "kid/ui.h"
#include "kid_platform.h"

#define TAG "KidDisplay"

namespace {

constexpr uint32_t kPollMs = 500;
// Return to the launcher after the chat has been idle this long.
constexpr int kIdleTicksBeforeHome = 15000 / kPollMs;

bool ConversationActive(DeviceState s) {
    return s == kDeviceStateConnecting || s == kDeviceStateListening || s == kDeviceStateSpeaking;
}

}  // namespace

KidDisplay::KidDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height,
                       int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy,
                       kid::Platform* platform)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy),
      platform_(platform) {}

void KidDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();  // builds the xiaozhi chat UI on the active screen

    DisplayLockGuard lock(this);
    chat_screen_ = lv_screen_active();

    // Round "home" button over the chat UI, bottom-left, 52x52.
    home_button_ = lv_button_create(chat_screen_);
    lv_obj_set_size(home_button_, 52, 52);
    lv_obj_align(home_button_, LV_ALIGN_BOTTOM_LEFT, 6, -6);
    lv_obj_set_style_radius(home_button_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home_button_, lv_color_hex(0x4C9AFF), 0);
    lv_obj_set_style_bg_opa(home_button_, LV_OPA_80, 0);
    lv_obj_set_style_shadow_width(home_button_, 0, 0);
    lv_obj_add_flag(home_button_, LV_OBJ_FLAG_FLOATING);
    lv_obj_t* icon = lv_label_create(home_button_);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_label_set_text(icon, LV_SYMBOL_HOME);
    lv_obj_center(icon);
    lv_obj_add_event_cb(home_button_, OnHomeClicked, LV_EVENT_CLICKED, this);

    auto* kp = static_cast<KidPlatform*>(platform_);
    kp->on_start_chat = [this]() { ShowChatScreen(); };

    kid::ui::Start(*platform_);  // loads the launcher
    chat_visible_ = false;
    lv_timer_create(OnStateTimer, kPollMs, this);
    lv_timer_create(OnTouchTimer, 30, this);
    ESP_LOGI(TAG, "Kid launcher started");
}

void KidDisplay::SetTheme(Theme* theme) {
    // SpiLcdDisplay::SetTheme styles whatever screen is active; make sure that
    // is the chat screen and not the launcher.
    DisplayLockGuard lock(this);
    lv_obj_t* previous = lv_screen_active();
    if (chat_screen_ && previous != chat_screen_) lv_screen_load(chat_screen_);
    SpiLcdDisplay::SetTheme(theme);
    if (chat_screen_ && previous != chat_screen_) lv_screen_load(previous);
}

void KidDisplay::ShowChatScreen() {
    DisplayLockGuard lock(this);
    ShowChatScreenLocked();
}

void KidDisplay::ShowLauncher() {
    DisplayLockGuard lock(this);
    ShowLauncherLocked();
}

void KidDisplay::ShowChatScreenLocked() {
    if (!chat_screen_ || chat_visible_) return;
    lv_screen_load(chat_screen_);
    chat_visible_ = true;
    idle_ticks_ = 0;
}

void KidDisplay::ShowLauncherLocked() {
    if (!chat_visible_) return;
    kid::ui::Show();
    chat_visible_ = false;
}

void KidDisplay::OnHomeClicked(lv_event_t* e) {
    auto* self = static_cast<KidDisplay*>(lv_event_get_user_data(e));
    auto& app = Application::GetInstance();
    if (ConversationActive(app.GetDeviceState())) app.ToggleChatState();  // end the conversation
    self->ShowLauncherLocked();
}

// Watches the pointer: wakes the display on touch and logs touch positions
// at debug level (useful when checking the touch orientation).
void KidDisplay::OnTouchTimer(lv_timer_t* timer) {
    auto* self = static_cast<KidDisplay*>(lv_timer_get_user_data(timer));
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) continue;
        const bool pressed = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
        if (pressed == self->touch_down_) continue;
        self->touch_down_ = pressed;
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        ESP_LOGD("KidTouch", "%s x=%d y=%d", pressed ? "down" : "up", static_cast<int>(pt.x), static_cast<int>(pt.y));
        if (pressed && self->on_touch) self->on_touch();
    }
}

void KidDisplay::OnStateTimer(lv_timer_t* timer) {
    static_cast<KidDisplay*>(lv_timer_get_user_data(timer))->PollDeviceState();
}

// Runs in the LVGL task, so the display lock is already held.
void KidDisplay::PollDeviceState() {
    static_cast<KidPlatform*>(platform_)->MaybeStartTimeSync();

    const DeviceState state = Application::GetInstance().GetDeviceState();
    if (ConversationActive(state) || state == kDeviceStateWifiConfiguring || state == kDeviceStateUpgrading) {
        ShowChatScreenLocked();  // e.g. the wake word was heard while on the launcher
        idle_ticks_ = 0;
        return;
    }
    if (chat_visible_ && state == kDeviceStateIdle && ++idle_ticks_ >= kIdleTicksBeforeHome) {
        ShowLauncherLocked();
    }
}
