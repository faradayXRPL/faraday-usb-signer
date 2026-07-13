#include <Arduino.h>
#include <esp_system.h>

#include "display.h"
#include "theme.h"
#include "ui.h"
#include "usb_link.h"
#include "wallet.h"

void setup() {
    Serial.setRxBufferSize(16384);
    Serial.setTxBufferSize(8192);
    Serial.begin(115200);
    delay(300);
    Serial.printf("[boot] reset_reason=%d\n", static_cast<int>(esp_reset_reason()));

    wallet::beginEntropy();
    display::begin();

    if (esp_reset_reason() == ESP_RST_POWERON) {
        delay(100);
        esp_restart();
    }

    wallet::begin();
    theme::init();
    ui::build();
    ui::playSplash();
    usblink::begin();
}

void loop() {
    usblink::tick();
    ui::tick();
    display::tick();
    delay(5);
}
