#include "crypto_util.h"

#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>

#include <cstring>

namespace crypto {

void bytesToHex(const uint8_t* data, size_t len, char* out, size_t outSize) {
    static const char hex[] = "0123456789ABCDEF";
    if (outSize < len * 2 + 1) return;
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = hex[data[i] >> 4];
        out[i * 2 + 1] = hex[data[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    mbedtls_sha256_ret(data, len, out, 0);
}

void sha512Half(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint8_t hash[64];
    mbedtls_sha512_ret(data, len, hash, 0);
    memcpy(out, hash, 32);
}

void sha512HalfHex(const uint8_t* data, size_t len, char* out, size_t outSize) {
    uint8_t half[32];
    sha512Half(data, len, half);
    bytesToHex(half, sizeof(half), out, outSize);
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hexToBytes(const char* hex, uint8_t* out, size_t outLen) {
    if (strlen(hex) != outLen * 2) return false;
    for (size_t i = 0; i < outLen; ++i) {
        const int hi = hexNibble(hex[2 * i]);
        const int lo = hexNibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

void putU32BE(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}

}  // namespace crypto
