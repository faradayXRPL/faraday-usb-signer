#pragma once

#include <cstddef>
#include <cstdint>

namespace wallet {

void begin();

// Storage / lock state.
bool hasWallet();   // a sealed wallet exists in NVS (may still be locked)
bool isUnlocked();  // seed is decrypted in RAM; signing/export enabled
int failedAttempts();  // consecutive wrong-PIN attempts since last success

// PIN lifecycle. The PIN is mixed into the storage key (PBKDF2); the seed can
// only be decrypted with the correct PIN on this device.
bool unlock(const char* pin);   // decrypt seed into RAM; false on wrong PIN
void lock();                    // zeroize the in-RAM seed
bool createWallet(const char* pin);
bool importSeed(const char* familySeed, const char* pin);
bool changePin(const char* newPin);  // re-seal current seed (must be unlocked)
bool clearWallet();

// Account info (only meaningful while unlocked).
const char* address();
const char* publicKeyHex();

// One-time backup: base58-encode the current seed into `out` (family seed,
// ~29 chars). Computed on demand from the in-RAM seed; never cached. Returns
// false if the wallet is locked.
bool exportFamilySeed(char* out, size_t outSize);

bool signSha512Half(const uint8_t* data, size_t len, uint8_t* der, size_t* derLen);
bool accountIdFromAddress(const char* address, uint8_t out[20]);
void shortenAddress(const char* full, char* out, size_t outSize);

}  // namespace wallet
