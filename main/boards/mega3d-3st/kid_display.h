// LCD display for the MEGA3D 3ST that hosts the kid apps (components/kid_apps)
// next to the stock xiaozhi chat screen.
//
// - The launcher is the home screen; the xiaozhi UI built by SpiLcdDisplay
//   becomes the "chat screen", shown only while chatting or configuring Wi-Fi.
// - While a conversation is active (wake word or chat tile) the chat screen is
//   shown; after it has been idle for a while the launcher comes back.
#pragma once

#include <lvgl.h>

#include <functional>

#include "display/lcd_display.h"
#include "kid/platform.h"

class KidDisplay : public SpiLcdDisplay {
public:
    KidDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height,
               int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy, kid::Platform* platform);

    void SetupUI() override;
    void SetTheme(Theme* theme) override;

    // Switch to the stock chat UI / back to the launcher (LVGL lock taken inside).
    void ShowChatScreen();
    void ShowLauncher();

    // Called (from the LVGL task) whenever the screen is touched, so the board
    // can wake the dimmed display.
    std::function<void()> on_touch;

    // Screen off: panel off plus a transparent shield on the top layer, so the
    // tap that wakes the screen does not also press what is underneath.
    void SetScreenOff(bool off);

private:
    kid::Platform* platform_;
    lv_obj_t* chat_screen_ = nullptr;
    lv_obj_t* home_button_ = nullptr;
    bool chat_visible_ = false;
    int idle_ticks_ = 0;

    bool touch_down_ = false;
    lv_obj_t* wake_shield_ = nullptr;

    static void OnStateTimer(lv_timer_t* timer);
    static void OnTouchTimer(lv_timer_t* timer);
    static void OnHomeClicked(lv_event_t* e);
    void PollDeviceState();
    void ShowChatScreenLocked();
    void ShowLauncherLocked();
};
