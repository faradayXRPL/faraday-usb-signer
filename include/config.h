#pragma once

#include <cstdint>

namespace cfg {

inline constexpr uint16_t SCREEN_WIDTH = 480;
inline constexpr uint16_t SCREEN_HEIGHT = 320;
inline constexpr uint8_t DISPLAY_ROTATION = 1;

// Layout tokens for 480x320 landscape.
inline constexpr int16_t UI_EDGE = 10;
inline constexpr int16_t UI_PAD = 16;
inline constexpr int16_t UI_BTN_H = 44;
inline constexpr int16_t UI_TOP_BAR_H = 44;
inline constexpr int16_t UI_QR_SIDE = 276;

inline constexpr uint16_t PANEL_WIDTH = SCREEN_WIDTH - 2 * UI_EDGE;
inline constexpr uint16_t PANEL_HEIGHT = SCREEN_HEIGHT - 2 - 2 * UI_EDGE;

inline constexpr bool DISPLAY_COLOR_INVERT = true;
inline constexpr int16_t DISPLAY_COL_OFFSET = 0;
inline constexpr int16_t DISPLAY_ROW_OFFSET = 0;
inline constexpr bool DIAGNOSTIC_BOOT_TEST = false;
inline constexpr bool ENTROPY_DIAGNOSTICS = false;

inline constexpr uint16_t LVGL_BUFFER_LINES = 40;

inline constexpr int PIN_BL_PRIMARY = 6;
inline constexpr int BL_LEDC_CHANNEL = 0;
inline constexpr uint32_t BL_LEDC_FREQ_HZ = 5000;
inline constexpr uint8_t BL_LEDC_RESOLUTION_BITS = 8;
inline constexpr uint8_t BL_BRIGHTNESS = 255;

inline constexpr int PIN_I2C_SCL = 7;
inline constexpr int PIN_I2C_SDA = 8;
inline constexpr uint8_t FT6336_I2C_ADDR = 0x38;

inline constexpr bool TOUCH_SWAP_XY = true;
inline constexpr bool TOUCH_MIRROR_X = false;
inline constexpr bool TOUCH_MIRROR_Y = true;

// Camera power-down bit on the IO expander. The signer never uses the camera,
// so display::begin() holds it powered down to save current.
inline constexpr uint8_t EXIO_CAM_PWDN = 0;
inline constexpr uint8_t EXIO_LCD_RST = 1;
inline constexpr uint8_t EXIO_LCD_CS = 2;
inline constexpr uint8_t DISPLAY_SPI_MODE = 0;

inline constexpr size_t MAX_TX_JSON = 4096;

inline constexpr char FW_NAME[] = "FARADAY";
inline constexpr char FW_VERSION[] = "v0.2";

inline constexpr int PIN_MIN_LEN = 4;
inline constexpr int PIN_MAX_LEN = 8;
inline constexpr int MAX_PIN_FAILS = 10;
inline constexpr uint32_t AUTO_LOCK_MS = 120000;

}  // namespace cfg
