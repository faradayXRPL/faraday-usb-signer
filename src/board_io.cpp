#include "board_io.h"

#include <Arduino.h>
#include <Wire.h>

#include "config.h"

namespace board_io {
namespace {

constexpr uint8_t TCA_ADDR = 0x20;
constexpr uint8_t REG_INPUT = 0x00;
constexpr uint8_t REG_OUTPUT = 0x01;
constexpr uint8_t REG_CONFIG = 0x03;

bool writeReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(TCA_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

uint8_t readReg(uint8_t reg) {
    Wire.beginTransmission(TCA_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0xFF;
    if (Wire.requestFrom(static_cast<int>(TCA_ADDR), 1) != 1) return 0xFF;
    return Wire.read();
}

}  // namespace

bool probeI2c(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

void setExio(uint8_t pin, bool high) {
    if (pin > 7) return;
    uint8_t level = readReg(REG_OUTPUT);
    if (high) {
        level |= static_cast<uint8_t>(1u << pin);
    } else {
        level &= static_cast<uint8_t>(~(1u << pin));
    }
    writeReg(REG_OUTPUT, level);
}

bool readExio(uint8_t pin) {
    if (pin > 7) return false;
    const uint8_t level = readReg(REG_INPUT);
    return (level & static_cast<uint8_t>(1u << pin)) != 0;
}

void initI2c() {
    Wire.begin(cfg::PIN_I2C_SDA, cfg::PIN_I2C_SCL);
    Wire.setClock(100000);
    delay(10);
}

void begin() {
    // Unused — display uses TCA9554 library directly (reference board pattern).
}

}  // namespace board_io
