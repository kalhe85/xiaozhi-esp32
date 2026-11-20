#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// DB MEGA3D 3ST (ESP32-S3 N16R8, 2.0" touch LCD).
// Pins recovered from the stock firmware (board "ostb-xiaozhi-3st"):
// live IO_MUX / GPIO-matrix reads over USB-JTAG plus disassembly of the
// stock board init code.

#include <driver/gpio.h>
#include <driver/spi_master.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_INPUT_REFERENCE    true

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_5
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_16
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_6
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_7

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_4
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_12
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_11
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR   // 0x30
#define AUDIO_CODEC_ES7210_ADDR  ES7210_CODEC_DEFAULT_ADDR   // 0x80

// Buttons: main (power) button is active-high, volume buttons active-low.
#define MAIN_BUTTON_GPIO         GPIO_NUM_1
#define VOLUME_UP_BUTTON_GPIO    GPIO_NUM_40
#define VOLUME_DOWN_BUTTON_GPIO  GPIO_NUM_39

// Battery: divider on ADC2 channel 6 (GPIO17); charger status low while charging.
#define BATTERY_ADC_UNIT         ADC_UNIT_2
#define BATTERY_ADC_CHANNEL      ADC_CHANNEL_6
#define BATTERY_CHARGING_PIN     GPIO_NUM_47

// LCD: JD9853 panel (240x296 native) driven through the NV3023 driver with
// the vendor init sequence; used in landscape.
#define DISPLAY_SPI_HOST         SPI3_HOST
#define DISPLAY_SPI_SCLK_PIN     GPIO_NUM_9
#define DISPLAY_SPI_MOSI_PIN     GPIO_NUM_10
#define DISPLAY_SPI_CS_PIN       GPIO_NUM_14
#define DISPLAY_DC_PIN           GPIO_NUM_8
#define DISPLAY_RST_PIN          GPIO_NUM_18
#define DISPLAY_SPI_PCLK_HZ      (40 * 1000 * 1000)

#define DISPLAY_WIDTH            296
#define DISPLAY_HEIGHT           240
#define DISPLAY_MIRROR_X         false
#define DISPLAY_MIRROR_Y         true
#define DISPLAY_SWAP_XY          true
#define DISPLAY_OFFSET_X         24
#define DISPLAY_OFFSET_Y         0
#define DISPLAY_INVERT_COLOR     false

#define DISPLAY_BACKLIGHT_PIN            GPIO_NUM_13
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT  false

// Touch: CST816D on the shared I2C bus, no INT/RST lines (polled).
#define TOUCH_I2C_ADDR           0x15
// The CST816D already reports landscape coordinates matching the display
// (x 0..295, y 0..239); verified with logged touches on 2026-11-19 and the
// stock firmware's touch config (x_max 296, y_max 240, no swap/mirror).
#define TOUCH_SWAP_XY            false
#define TOUCH_MIRROR_X           false
#define TOUCH_MIRROR_Y           false

#endif  // _BOARD_CONFIG_H_
