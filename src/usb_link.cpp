#include "usb_link.h"

#include <Arduino.h>

#include <cstdlib>
#include <cstring>

#include "config.h"
#include "ui.h"
#include "wallet.h"

namespace usblink {
namespace {

enum class RxMode { Line, Payload };

char g_lineBuf[64];
size_t g_lineLen = 0;

char g_payloadBuf[cfg::MAX_TX_JSON + 1];
size_t g_payloadLen = 0;
size_t g_expectedLen = 0;
RxMode g_mode = RxMode::Line;

uint32_t g_lastHostMs = 0;
constexpr uint32_t kHostTimeoutMs = 4000;

void markHostSeen() { g_lastHostMs = millis(); }

void deliverUnsigned(const char* json) {
    if (!json || !json[0]) {
        emitError("Empty payload");
        return;
    }
    if (!wallet::isUnlocked()) {
        emitError("Wallet locked");
        return;
    }
    ui::submitUnsignedTx(json);
    Serial.println("ACK");
}

void handleUnsignedJson(const char* json) {
    deliverUnsigned(json);
}

void startPayload(size_t len) {
    if (len == 0 || len > cfg::MAX_TX_JSON) {
        emitError("Bad length");
        g_mode = RxMode::Line;
        g_payloadLen = 0;
        g_expectedLen = 0;
        return;
    }
    g_expectedLen = len;
    g_payloadLen = 0;
    g_mode = RxMode::Payload;
}

void finishPayload() {
    g_payloadBuf[g_payloadLen] = '\0';
    const char* json = g_payloadBuf;
    g_mode = RxMode::Line;
    g_expectedLen = 0;
    g_payloadLen = 0;
    handleUnsignedJson(json);
}

void handleLine(const char* line) {
    if (strncmp(line, "UNSIGNED ", 9) == 0) {
        const char* rest = line + 9;
        while (*rest == ' ') ++rest;
        if (!rest[0]) {
            emitError("Empty payload");
            return;
        }
        if (rest[0] == '{') {
            handleUnsignedJson(rest);
            return;
        }
        char* end = nullptr;
        const unsigned long len = strtoul(rest, &end, 10);
        while (end && (*end == ' ')) ++end;
        if (end && *end == '{') {
            handleUnsignedJson(end);
            return;
        }
        startPayload(static_cast<size_t>(len));
        return;
    }

    if (strcmp(line, "PING") == 0) {
        Serial.println(wallet::isUnlocked() ? "PONG unlocked" : "PONG locked");
        return;
    }

    if (strcmp(line, "ADDRESS") == 0) {
        if (!wallet::hasWallet()) {
            Serial.println("ADDRESS none");
        } else if (!wallet::isUnlocked()) {
            Serial.println("ADDRESS locked");
        } else {
            Serial.print("ADDRESS ");
            Serial.println(wallet::address());
        }
        return;
    }
}

void readPayloadBytes() {
    while (Serial.available() && g_payloadLen < g_expectedLen) {
        g_payloadBuf[g_payloadLen++] = static_cast<char>(Serial.read());
    }
    if (g_payloadLen < g_expectedLen) return;

    while (Serial.available()) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\n' || c == '\r') break;
    }
    finishPayload();
}

void readLineBytes() {
    while (Serial.available()) {
        if (g_mode == RxMode::Payload) return;

        const char c = static_cast<char>(Serial.read());
        if (c == '\n' || c == '\r') {
            if (g_lineLen == 0) continue;
            g_lineBuf[g_lineLen] = '\0';
            handleLine(g_lineBuf);
            g_lineLen = 0;
            if (g_mode == RxMode::Payload) return;
            continue;
        }
        if (g_lineLen < sizeof(g_lineBuf) - 1) {
            g_lineBuf[g_lineLen++] = c;
            continue;
        }
        g_lineLen = 0;
        emitError("Header too long");
        while (Serial.available()) {
            const char skip = static_cast<char>(Serial.read());
            if (skip == '\n' || skip == '\r') break;
        }
    }
}

}  // namespace

void begin() {
    g_lineLen = 0;
    g_payloadLen = 0;
    g_expectedLen = 0;
    g_mode = RxMode::Line;
    Serial.println("READY usb-signer");
}

void tick() {
    if (Serial.available()) markHostSeen();
    if (g_mode == RxMode::Payload) {
        readPayloadBytes();
        return;
    }
    readLineBytes();
    if (g_mode == RxMode::Payload) readPayloadBytes();
}

bool hostActive() {
    return g_lastHostMs != 0 && (millis() - g_lastHostMs) < kHostTimeoutMs;
}

void emitSigned(const char* json) {
    if (!json) json = "";
    // Single-line JSON, written in small chunks so the host OS/driver never
    // has to satisfy a multi-kilobyte read in one syscall.
    Serial.print("SIGNED ");
    const size_t len = strlen(json);
    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > 64) n = 64;
        Serial.write(reinterpret_cast<const uint8_t*>(json + off), n);
        off += n;
        Serial.flush();
        delay(2);
    }
    Serial.println();
    Serial.flush();
}

void emitRejected() {
    Serial.println("REJECTED");
}

void emitError(const char* message) {
    Serial.print("ERROR ");
    Serial.println(message ? message : "Unknown");
}

}  // namespace usblink
