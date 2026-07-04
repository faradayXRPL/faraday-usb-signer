#pragma once

#include <cstdint>

namespace board_io {

void initI2c();
bool probeI2c(uint8_t addr);
void begin();

void setExio(uint8_t pin, bool high);
bool readExio(uint8_t pin);

}  // namespace board_io
