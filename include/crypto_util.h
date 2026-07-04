#pragma once

#include <cstddef>
#include <cstdint>

namespace crypto {

void bytesToHex(const uint8_t* data, size_t len, char* out, size_t outSize);
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);
void sha512Half(const uint8_t* data, size_t len, uint8_t out[32]);
void sha512HalfHex(const uint8_t* data, size_t len, char* out, size_t outSize);

int hexNibble(char c);
bool hexToBytes(const char* hex, uint8_t* out, size_t outLen);

void putU32BE(uint8_t* out, uint32_t value);

}  // namespace crypto
