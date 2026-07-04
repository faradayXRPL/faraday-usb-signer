#include "power.h"

#include <Arduino.h>
#include <Wire.h>

#include "config.h"

namespace power {
namespace {

constexpr uint8_t AXP_ADDR = 0x34;

bool writeReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(AXP_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

}  // namespace

void begin() {
    Wire.beginTransmission(AXP_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.printf("[power] AXP2101 @0x%02x not found\n", AXP_ADDR);
        return;
    }

    writeReg(0x22, 0b110);
    writeReg(0x27, 0x10);
    writeReg(0x80, 0x01);
    writeReg(0x90, 0x00);
    writeReg(0x91, 0x00);
    writeReg(0x82, static_cast<uint8_t>((3300 - 1500) / 100));
    writeReg(0x92, static_cast<uint8_t>((3300 - 500) / 100));
    writeReg(0x96, static_cast<uint8_t>((1500 - 500) / 100));
    writeReg(0x97, static_cast<uint8_t>((2800 - 500) / 100));
    writeReg(0x90, 0x31);
    writeReg(0x64, 0x02);
    writeReg(0x61, 0x02);
    writeReg(0x62, 0x08);
    writeReg(0x63, 0x01);
    delay(200);
    Serial.println("[power] AXP2101 configured");
}

}  // namespace power
