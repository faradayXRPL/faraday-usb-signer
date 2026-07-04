#pragma once

#include <cstdint>

// Display + touch bring-up for ESP32-S3 480×320 touch LCD boards.
// Wraps Arduino_GFX (ST7796), FT6336 touch and the LVGL HAL glue.

namespace display {

void begin();
void tick();

// Set LCD backlight duty (0-255). Used to reduce current draw during camera use.
void setBacklight(uint8_t duty);

}  // namespace display
