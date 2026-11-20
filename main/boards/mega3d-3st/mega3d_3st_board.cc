// DB MEGA3D 3ST: ESP32-S3 N16R8, JD9853 240x296 LCD (landscape), CST816D touch,
// ES8311 + ES7210 audio, ADC battery sense. See config.h for pin provenance.

#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "power_save_timer.h"
#include "adc_battery_monitor.h"
#include "assets/lang_config.h"
#include "kid_display.h"
#include "kid_platform.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_nv3023.h>
#include <esp_lcd_touch_cst816s.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <ssid_manager.h>
#include <wifi_manager.h>
#include <cstdlib>
#include <ctime>

#define TAG "Mega3d3st"

namespace {

// Vendor init sequence for the JD9853 panel, copied from the stock firmware.
// The NV3023 driver only sends these commands; MADCTL (0x36) and display-on
// are issued afterwards through esp_lcd_panel_* calls.
const nv3023_lcd_init_cmd_t kLcdInitCmds[] = {
    {0xDF, (uint8_t[]){0x98, 0x53}, 2, 0},
    {0xDE, (uint8_t[]){0x00}, 1, 0},
    {0xB2, (uint8_t[]){0x25}, 1, 0},
    {0xB7, (uint8_t[]){0x00, 0x29, 0x00, 0x51}, 4, 0},
    {0xBB, (uint8_t[]){0x4F, 0x1A, 0x55, 0x73, 0x63, 0xF0}, 6, 0},
    {0xC0, (uint8_t[]){0x44, 0xA4}, 2, 0},
    {0xC1, (uint8_t[]){0x12}, 1, 0},
    {0xC3, (uint8_t[]){0x7D, 0x07, 0x14, 0x06, 0xC8, 0x71, 0x6C, 0x77}, 8, 0},
    {0xC4, (uint8_t[]){0x00, 0x00, 0x94, 0x79, 0x25, 0x0A, 0x16, 0x79, 0x25, 0x0A, 0x16, 0x82}, 12, 0},
    {0xC8, (uint8_t[]){0x3F, 0x34, 0x2D, 0x26, 0x2B, 0x2B, 0x25, 0x24, 0x23, 0x22, 0x20, 0x17, 0x14, 0x0E, 0x06, 0x00,
                       0x3F, 0x34, 0x2D, 0x26, 0x2B, 0x2B, 0x25, 0x24, 0x23, 0x22, 0x20, 0x17, 0x14, 0x0E, 0x06, 0x00}, 32, 0},
    {0xD0, (uint8_t[]){0x04, 0x06, 0x6B, 0x0F, 0x00}, 5, 0},
    {0xD7, (uint8_t[]){0x00, 0x30}, 2, 0},
    {0xE6, (uint8_t[]){0x10}, 1, 0},
    {0xDE, (uint8_t[]){0x01}, 1, 0},
    {0xB7, (uint8_t[]){0x03, 0x13, 0xEF, 0x35, 0x35}, 5, 0},
    {0xC1, (uint8_t[]){0x14, 0x15, 0xC0}, 3, 0},
    {0xC2, (uint8_t[]){0x06, 0x3A}, 2, 0},
    {0xC4, (uint8_t[]){0x72, 0x12}, 2, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xDE, (uint8_t[]){0x00}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x05}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0x27}, 4, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
};

}  // namespace

class Mega3d3stBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button main_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    KidDisplay* display_ = nullptr;
    KidPlatform kid_platform_;
    // Only open the Wi-Fi setup hotspot when someone asked for it (settings
    // menu); otherwise the device stays offline on the launcher.
    bool wifi_setup_requested_ = false;
    PowerSaveTimer* power_save_timer_ = nullptr;
    AdcBatteryMonitor* battery_monitor_ = nullptr;

    static bool IsCharging() {
        return gpio_get_level(BATTERY_CHARGING_PIN) == 0;
    }

    void InitializeBattery() {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << BATTERY_CHARGING_PIN;
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        ESP_ERROR_CHECK(gpio_config(&cfg));
        // Divider ratio assumed 1:1; compare the reported level with the stock
        // firmware before relying on it.
        battery_monitor_ = new AdcBatteryMonitor(BATTERY_ADC_UNIT, BATTERY_ADC_CHANNEL, 100000, 100000, GPIO_NUM_NC);
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(10);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->SetEnabled(!IsCharging());
    }

    void InitializeI2c() {
        i2c_master_bus_config_t cfg = {};
        cfg.i2c_port = I2C_NUM_0;
        cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        cfg.glitch_ignore_cnt = 7;
        cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t cfg = {};
        cfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        cfg.miso_io_num = GPIO_NUM_NC;
        cfg.sclk_io_num = DISPLAY_SPI_SCLK_PIN;
        cfg.quadwp_io_num = GPIO_NUM_NC;
        cfg.quadhd_io_num = GPIO_NUM_NC;
        cfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &cfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = DISPLAY_SPI_PCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io));

        nv3023_vendor_config_t vendor_config = {};
        vendor_config.init_cmds = kLcdInitCmds;
        vendor_config.init_cmds_size = sizeof(kLcdInitCmds) / sizeof(kLcdInitCmds[0]);

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = &vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_nv3023(panel_io, &panel_config, &panel));

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        vTaskDelay(pdMS_TO_TICKS(15));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));

        display_ = new KidDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                  DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, &kid_platform_);
        // Touching the screen counts as activity, like pressing a button.
        display_->on_touch = [this]() {
            if (power_save_timer_) power_save_timer_->WakeUp();
        };
    }

    void InitializeTouch() {
        esp_lcd_panel_io_handle_t tp_io = nullptr;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {};
        tp_io_config.dev_addr = TOUCH_I2C_ADDR;
        tp_io_config.scl_speed_hz = 400 * 1000;
        tp_io_config.control_phase_bytes = 1;
        tp_io_config.dc_bit_offset = 0;
        tp_io_config.lcd_cmd_bits = 8;
        tp_io_config.lcd_param_bits = 0;
        tp_io_config.flags.disable_control_phase = 1;
        if (esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io) != ESP_OK) {
            ESP_LOGE(TAG, "Touch I2C IO init failed");
            return;
        }

        esp_lcd_touch_config_t tp_cfg = {};
        tp_cfg.x_max = TOUCH_SWAP_XY ? DISPLAY_HEIGHT : DISPLAY_WIDTH;
        tp_cfg.y_max = TOUCH_SWAP_XY ? DISPLAY_WIDTH : DISPLAY_HEIGHT;
        tp_cfg.rst_gpio_num = GPIO_NUM_NC;
        tp_cfg.int_gpio_num = GPIO_NUM_NC;
        tp_cfg.flags.swap_xy = TOUCH_SWAP_XY;
        tp_cfg.flags.mirror_x = TOUCH_MIRROR_X;
        tp_cfg.flags.mirror_y = TOUCH_MIRROR_Y;

        esp_lcd_touch_handle_t tp = nullptr;
        if (esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &tp) != ESP_OK) {
            ESP_LOGE(TAG, "CST816D touch init failed");
            return;
        }
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch initialized");
    }

    void ChangeVolume(int delta) {
        power_save_timer_->WakeUp();
        auto codec = GetAudioCodec();
        int volume = codec->output_volume() + delta;
        if (volume < 0) volume = 0;
        if (volume > 100) volume = 100;
        codec->SetOutputVolume(volume);
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
    }

    void InitializeButtons() {
        // Main button: talk to the chatbot when it is reachable; otherwise go home.
        main_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            if (kid_platform_.IsOnline()) {
                display_->ShowChatScreen();
                Application::GetInstance().ToggleChatState();
            } else {
                display_->ShowLauncher();
            }
        });

        // Holding a volume button behaves like a click: no jump to 100% or mute
        // on a child's device.
        volume_up_button_.OnClick([this]() { ChangeVolume(10); });
        volume_up_button_.OnLongPress([this]() { ChangeVolume(10); });
        volume_down_button_.OnClick([this]() { ChangeVolume(-10); });
        volume_down_button_.OnLongPress([this]() { ChangeVolume(-10); });
    }

protected:
    // Called by WifiBoard when there is no saved network or the connection
    // timed out. Stay on the launcher and keep trying in the background
    // instead of blocking the device on the setup hotspot.
    void StartWifiConfigMode() override {
        if (wifi_setup_requested_) {
            WifiBoard::StartWifiConfigMode();
            return;
        }
        in_config_mode_ = false;
        if (!SsidManager::GetInstance().GetSsidList().empty()) {
            ESP_LOGW(TAG, "Wi-Fi not reachable, staying offline and retrying");
            WifiManager::GetInstance().StartStation();
        } else {
            ESP_LOGW(TAG, "No saved Wi-Fi, staying offline");
        }
    }

public:
    // Opens the stock Wi-Fi setup hotspot (from the kid settings screen).
    void OpenWifiSetup() {
        wifi_setup_requested_ = true;
        EnterWifiConfigMode();
    }

    Mega3d3stBoard()
        : main_button_(MAIN_BUTTON_GPIO, true),
          volume_up_button_(VOLUME_UP_BUTTON_GPIO),
          volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        // time() is kept in UTC (CONFIG_SYSTEM_TIME_IS_UTC); local time is ICT.
        setenv("TZ", "ICT-7", 1);
        tzset();
        kid_platform_.Initialize();
        InitializeBattery();
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }

    AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                         AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                                         AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
                                         AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    Display* GetDisplay() override { return display_; }

    Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = IsCharging();
        discharging = !charging;
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = battery_monitor_->GetBatteryLevel();
        return true;
    }

    void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(Mega3d3stBoard);
