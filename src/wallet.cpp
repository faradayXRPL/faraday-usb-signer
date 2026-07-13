#include "wallet.h"

#include <Arduino.h>
#include <Preferences.h>
#include <bootloader_random.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <mbedtls/asn1write.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "crypto_util.h"

namespace wallet {
namespace {

constexpr char PREF_NS[] = "aqwallet";
constexpr char PREF_BLOB[] = "seed_gcm";
constexpr char PREF_SALT[] = "kdf_salt";
constexpr char PREF_FAILS[] = "pin_fails";
constexpr char STORAGE_AAD[] = "xrpl-airgap-seed-v1";
constexpr size_t SEED_LEN = 16;
constexpr size_t NONCE_LEN = 12;
constexpr size_t TAG_LEN = 16;
constexpr size_t BLOB_LEN = NONCE_LEN + SEED_LEN + TAG_LEN;
constexpr size_t SALT_LEN = 16;
constexpr uint32_t PBKDF2_ITERS = 50000;  // PIN stretch (PBKDF2-HMAC-SHA256)
constexpr char XRPL_ALPHABET[] =
    "rpshnaf39wBUDNEGHJKLM4PQRST7VWXYZ2bcdeCg65jkm8oFqi1tuvAxyz";

Preferences prefs;
bool g_hasWallet = false;  // a sealed blob + salt exist in NVS
bool g_unlocked = false;   // seed is decrypted in RAM (signing enabled)
int g_fails = 0;           // consecutive wrong-PIN attempts
bool g_entropyBootstrapped = false;
uint8_t g_salt[SALT_LEN] = {};
// The 16-byte seed is held decrypted in RAM ONLY while unlocked, and is
// zeroized on lock()/clearWallet(). At rest it is sealed with AES-256-GCM under
// a key derived via PBKDF2 from (deviceMAC || PIN) + per-wallet salt, so a flash
// dump alone cannot recover it. The family-seed string is never cached.
uint8_t g_seed[SEED_LEN] = {};
char g_address[40] = "NO WALLET";
char g_publicKeyHex[68] = "";

void beginEntropyInternal() {
    if (g_entropyBootstrapped) return;
    bootloader_random_enable();
    for (int i = 0; i < 32; ++i) {
        (void)esp_random();
        delayMicroseconds(20);
    }
    g_entropyBootstrapped = true;
}

bool sha256Update(mbedtls_sha256_context* ctx, const void* data, size_t len) {
    return len == 0 ||
           mbedtls_sha256_update_ret(ctx, static_cast<const unsigned char*>(data), len) == 0;
}

bool fillEntropy(uint8_t* out, size_t len, const char* label) {
    if (!out && len) return false;
    beginEntropyInternal();

    uint8_t prev[32] = {};
    size_t off = 0;
    uint32_t counter = 0;
    while (off < len) {
        uint8_t hw[64];
        esp_fill_random(hw, sizeof(hw));

        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        bool ok = mbedtls_sha256_starts_ret(&ctx, 0) == 0 &&
                  sha256Update(&ctx, label, strlen(label)) &&
                  sha256Update(&ctx, &counter, sizeof(counter)) &&
                  sha256Update(&ctx, prev, sizeof(prev)) &&
                  sha256Update(&ctx, hw, sizeof(hw));
        for (int i = 0; ok && i < 24; ++i) {
            const uint32_t rnd = esp_random();
            const int64_t us = esp_timer_get_time();
            const uint32_t arduinoUs = micros();
            ok = sha256Update(&ctx, &rnd, sizeof(rnd)) &&
                 sha256Update(&ctx, &us, sizeof(us)) &&
                 sha256Update(&ctx, &arduinoUs, sizeof(arduinoUs));
            delayMicroseconds((rnd & 0x03U) + 1U);
        }
        ok = ok && mbedtls_sha256_finish_ret(&ctx, prev) == 0;
        mbedtls_sha256_free(&ctx);
        memset(hw, 0, sizeof(hw));
        if (!ok) {
            memset(out, 0, len);
            memset(prev, 0, sizeof(prev));
            return false;
        }

        const size_t n = (len - off) < sizeof(prev) ? (len - off) : sizeof(prev);
        memcpy(out + off, prev, n);
        off += n;
        ++counter;
    }
    memset(prev, 0, sizeof(prev));
    return true;
}

int espRng(void*, unsigned char* out, size_t len) {
    return fillEntropy(out, len, "mbedtls-rng") ? 0 : -1;
}

bool hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len,
                uint8_t out[32]) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info != nullptr && mbedtls_md_hmac(info, key, keyLen, data, len, out) == 0;
}

bool hmacSha256Chunks(const uint8_t* key, size_t keyLen, const uint8_t* a, size_t aLen,
                      const uint8_t* b, size_t bLen, const uint8_t* c, size_t cLen,
                      const uint8_t* d, size_t dLen, uint8_t out[32]) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    bool ok = info != nullptr && mbedtls_md_setup(&md, info, 1) == 0 &&
              mbedtls_md_hmac_starts(&md, key, keyLen) == 0;
    if (ok && aLen) ok = mbedtls_md_hmac_update(&md, a, aLen) == 0;
    if (ok && bLen) ok = mbedtls_md_hmac_update(&md, b, bLen) == 0;
    if (ok && cLen) ok = mbedtls_md_hmac_update(&md, c, cLen) == 0;
    if (ok && dLen) ok = mbedtls_md_hmac_update(&md, d, dLen) == 0;
    ok = ok && mbedtls_md_hmac_finish(&md, out) == 0;
    mbedtls_md_free(&md);
    return ok;
}

bool derEncodeEcdsa(const mbedtls_mpi* r, const mbedtls_mpi* s, uint8_t* der, size_t* derLen) {
    uint8_t buf[96];
    uint8_t* const end = buf + sizeof(buf);
    uint8_t* p = end;
    size_t contentLen = 0;
    // mbedTLS ASN.1 writers prepend into the buffer, so write s before r to
    // produce the DER sequence in the required r, s order.
    int n = mbedtls_asn1_write_mpi(&p, buf, s);
    if (n <= 0) return false;
    contentLen += static_cast<size_t>(n);
    n = mbedtls_asn1_write_mpi(&p, buf, r);
    if (n <= 0) return false;
    contentLen += static_cast<size_t>(n);
    n = mbedtls_asn1_write_len(&p, buf, contentLen);
    if (n <= 0) return false;
    contentLen += static_cast<size_t>(n);
    n = mbedtls_asn1_write_tag(&p, buf, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
    if (n <= 0) return false;
    contentLen += static_cast<size_t>(n);
    const size_t derOutLen = end - p;
    if (contentLen != derOutLen || derOutLen == 0 || derOutLen > *derLen) return false;
    memcpy(der, p, derOutLen);
    *derLen = derOutLen;
    return true;
}

bool mpiFromHash(mbedtls_mpi* e, const mbedtls_ecp_group* grp, const uint8_t hash[32]) {
    return mbedtls_mpi_read_binary(e, hash, 32) == 0 && mbedtls_mpi_mod_mpi(e, e, &grp->N) == 0;
}

bool mpiToFixed32(const mbedtls_mpi* value, uint8_t out[32]) {
    return mbedtls_mpi_write_binary(value, out, 32) == 0;
}

bool bitsToOctets(const mbedtls_ecp_group* grp, const uint8_t hash[32], uint8_t out[32]) {
    mbedtls_mpi z;
    mbedtls_mpi_init(&z);
    const bool ok = mbedtls_mpi_read_binary(&z, hash, 32) == 0 &&
                    mbedtls_mpi_mod_mpi(&z, &z, &grp->N) == 0 && mpiToFixed32(&z, out);
    mbedtls_mpi_free(&z);
    return ok;
}

bool hmacInto(uint8_t K[32], const uint8_t* data, size_t len, uint8_t out[32]) {
    uint8_t tmp[32];
    const bool ok = hmacSha256(K, 32, data, len, tmp);
    if (ok) memcpy(out, tmp, sizeof(tmp));
    memset(tmp, 0, sizeof(tmp));
    return ok;
}

bool rfc6979Update(uint8_t K[32], uint8_t V[32], uint8_t sep, const uint8_t* seed,
                   size_t seedLen) {
    uint8_t nextK[32];
    uint8_t nextV[32];
    const bool okK = hmacSha256Chunks(K, 32, V, 32, &sep, 1, seed, seedLen, nullptr, 0, nextK);
    if (!okK) {
        memset(nextK, 0, sizeof(nextK));
        return false;
    }
    memcpy(K, nextK, sizeof(nextK));
    const bool okV = hmacInto(K, V, 32, nextV);
    if (okV) memcpy(V, nextV, sizeof(nextV));
    memset(nextK, 0, sizeof(nextK));
    memset(nextV, 0, sizeof(nextV));
    return okV;
}

bool hedgedRfc6979ScalarNonZero(mbedtls_mpi* k, const mbedtls_ecp_group* grp,
                                const uint8_t priv[32], const uint8_t hash[32]) {
    uint8_t h1[32];
    uint8_t hedge[32];
    uint8_t seed[96];
    uint8_t K[32];
    uint8_t V[32];
    uint8_t candidate[32];
    memset(K, 0x00, sizeof(K));
    memset(V, 0x01, sizeof(V));

    bool ok = bitsToOctets(grp, hash, h1) && fillEntropy(hedge, sizeof(hedge), "ecdsa-k-hedge");
    if (ok) {
        memcpy(seed, priv, 32);
        memcpy(seed + 32, h1, 32);
        memcpy(seed + 64, hedge, 32);
        ok = rfc6979Update(K, V, 0x00, seed, sizeof(seed)) &&
             rfc6979Update(K, V, 0x01, seed, sizeof(seed));
    }

    for (int attempt = 0; ok && attempt < 64; ++attempt) {
        uint8_t nextV[32];
        ok = hmacInto(K, V, 32, nextV);
        if (!ok) break;
        memcpy(V, nextV, sizeof(nextV));
        memcpy(candidate, V, sizeof(candidate));
        memset(nextV, 0, sizeof(nextV));

        if (mbedtls_mpi_read_binary(k, candidate, sizeof(candidate)) == 0 &&
            mbedtls_mpi_cmp_int(k, 0) > 0 && mbedtls_mpi_cmp_mpi(k, &grp->N) < 0) {
            memset(h1, 0, sizeof(h1));
            memset(hedge, 0, sizeof(hedge));
            memset(seed, 0, sizeof(seed));
            memset(K, 0, sizeof(K));
            memset(V, 0, sizeof(V));
            memset(candidate, 0, sizeof(candidate));
            return true;
        }
        ok = rfc6979Update(K, V, 0x00, nullptr, 0);
    }

    memset(h1, 0, sizeof(h1));
    memset(hedge, 0, sizeof(hedge));
    memset(seed, 0, sizeof(seed));
    memset(K, 0, sizeof(K));
    memset(V, 0, sizeof(V));
    memset(candidate, 0, sizeof(candidate));
    return false;
}

void normalizeLowS(mbedtls_mpi* s, const mbedtls_ecp_group* grp) {
    mbedtls_mpi halfN;
    mbedtls_mpi_init(&halfN);
    if (mbedtls_mpi_copy(&halfN, &grp->N) == 0 && mbedtls_mpi_shift_r(&halfN, 1) == 0 &&
        mbedtls_mpi_cmp_mpi(s, &halfN) > 0) {
        mbedtls_mpi_sub_mpi(s, &grp->N, s);
    }
    mbedtls_mpi_free(&halfN);
}

// Noble-compatible ECDSA on secp256k1: sign the 32-byte sha512Half digest directly.
bool ecdsaSignDigest(mbedtls_ecp_group* grp, const uint8_t priv[32], const uint8_t hash[32],
                     mbedtls_mpi* rOut, mbedtls_mpi* sOut) {
    mbedtls_mpi d, e, k, kinv, rd, sum;
    mbedtls_ecp_point rPoint;
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&k);
    mbedtls_mpi_init(&kinv);
    mbedtls_mpi_init(&rd);
    mbedtls_mpi_init(&sum);
    mbedtls_ecp_point_init(&rPoint);

    bool ok = mbedtls_mpi_read_binary(&d, priv, 32) == 0 && mpiFromHash(&e, grp, hash);
    if (ok) {
        ok = false;
        for (int attempt = 0; attempt < 64; ++attempt) {
            if (!hedgedRfc6979ScalarNonZero(&k, grp, priv, hash)) continue;
            if (mbedtls_ecp_mul(grp, &rPoint, &k, &grp->G, espRng, nullptr) != 0) continue;
            if (mbedtls_mpi_mod_mpi(rOut, &rPoint.X, &grp->N) != 0) continue;
            if (mbedtls_mpi_cmp_int(rOut, 0) == 0) continue;
            if (mbedtls_mpi_inv_mod(&kinv, &k, &grp->N) != 0) continue;
            if (mbedtls_mpi_mul_mpi(&rd, rOut, &d) != 0) continue;
            if (mbedtls_mpi_mod_mpi(&rd, &rd, &grp->N) != 0) continue;
            if (mbedtls_mpi_add_mpi(&sum, &e, &rd) != 0) continue;
            if (mbedtls_mpi_mod_mpi(&sum, &sum, &grp->N) != 0) continue;
            if (mbedtls_mpi_mul_mpi(sOut, &kinv, &sum) != 0) continue;
            if (mbedtls_mpi_mod_mpi(sOut, sOut, &grp->N) != 0) continue;
            if (mbedtls_mpi_cmp_int(sOut, 0) == 0) continue;
            normalizeLowS(sOut, grp);
            ok = true;
            break;
        }
    }

    mbedtls_ecp_point_free(&rPoint);
    mbedtls_mpi_free(&sum);
    mbedtls_mpi_free(&rd);
    mbedtls_mpi_free(&kinv);
    mbedtls_mpi_free(&k);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&d);
    return ok;
}

uint32_t rol(uint32_t x, uint8_t n) {
    return (x << n) | (x >> (32 - n));
}

uint32_t ripemdF(uint32_t j, uint32_t x, uint32_t y, uint32_t z) {
    if (j < 16) return x ^ y ^ z;
    if (j < 32) return (x & y) | (~x & z);
    if (j < 48) return (x | ~y) ^ z;
    if (j < 64) return (x & z) | (y & ~z);
    return x ^ (y | ~z);
}

uint32_t ripemdK(uint32_t j) {
    if (j < 16) return 0x00000000UL;
    if (j < 32) return 0x5A827999UL;
    if (j < 48) return 0x6ED9EBA1UL;
    if (j < 64) return 0x8F1BBCDCUL;
    return 0xA953FD4EUL;
}

uint32_t ripemdKp(uint32_t j) {
    if (j < 16) return 0x50A28BE6UL;
    if (j < 32) return 0x5C4DD124UL;
    if (j < 48) return 0x6D703EF3UL;
    if (j < 64) return 0x7A6D76E9UL;
    return 0x00000000UL;
}

void ripemd160(const uint8_t* data, size_t len, uint8_t out[20]) {
    static const uint8_t r[80] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
        7, 4, 13, 1, 10, 6, 15, 3, 12, 0, 9, 5, 2, 14, 11, 8,
        3, 10, 14, 4, 9, 15, 8, 1, 2, 7, 0, 6, 13, 11, 5, 12,
        1, 9, 11, 10, 0, 8, 12, 4, 13, 3, 7, 15, 14, 5, 6, 2,
        4, 0, 5, 9, 7, 12, 2, 10, 14, 1, 3, 8, 11, 6, 15, 13};
    static const uint8_t rp[80] = {
        5, 14, 7, 0, 9, 2, 11, 4, 13, 6, 15, 8, 1, 10, 3, 12,
        6, 11, 3, 7, 0, 13, 5, 10, 14, 15, 8, 12, 4, 9, 1, 2,
        15, 5, 1, 3, 7, 14, 6, 9, 11, 8, 12, 2, 10, 0, 4, 13,
        8, 6, 4, 1, 3, 11, 15, 0, 5, 12, 2, 13, 9, 7, 10, 14,
        12, 15, 10, 4, 1, 5, 8, 7, 6, 2, 13, 14, 0, 3, 9, 11};
    static const uint8_t s[80] = {
        11, 14, 15, 12, 5, 8, 7, 9, 11, 13, 14, 15, 6, 7, 9, 8,
        7, 6, 8, 13, 11, 9, 7, 15, 7, 12, 15, 9, 11, 7, 13, 12,
        11, 13, 6, 7, 14, 9, 13, 15, 14, 8, 13, 6, 5, 12, 7, 5,
        11, 12, 14, 15, 14, 15, 9, 8, 9, 14, 5, 6, 8, 6, 5, 12,
        9, 15, 5, 11, 6, 8, 13, 12, 5, 12, 13, 14, 11, 8, 5, 6};
    static const uint8_t sp[80] = {
        8, 9, 9, 11, 13, 15, 15, 5, 7, 7, 8, 11, 14, 14, 12, 6,
        9, 13, 15, 7, 12, 8, 9, 11, 7, 7, 12, 7, 6, 15, 13, 11,
        9, 7, 15, 11, 8, 6, 6, 14, 12, 13, 5, 14, 13, 13, 7, 5,
        15, 5, 8, 11, 14, 14, 6, 14, 6, 9, 12, 9, 12, 5, 15, 8,
        8, 5, 12, 9, 12, 5, 14, 6, 8, 13, 6, 5, 15, 13, 11, 11};

    uint32_t h0 = 0x67452301UL, h1 = 0xEFCDAB89UL, h2 = 0x98BADCFEUL;
    uint32_t h3 = 0x10325476UL, h4 = 0xC3D2E1F0UL;
    const uint64_t bitLen = static_cast<uint64_t>(len) * 8;
    const size_t paddedLen = ((len + 9 + 63) / 64) * 64;
    uint8_t block[64];
    for (size_t offset = 0; offset < paddedLen; offset += 64) {
        memset(block, 0, sizeof(block));
        if (offset < len) {
            const size_t copyLen = (len - offset) > 64 ? 64 : (len - offset);
            memcpy(block, data + offset, copyLen);
        }
        if (offset <= len && len < offset + 64) block[len - offset] = 0x80;
        if (offset + 64 == paddedLen) {
            for (int i = 0; i < 8; ++i) block[56 + i] = static_cast<uint8_t>((bitLen >> (8 * i)) & 0xFF);
        }
        uint32_t x[16];
        for (int i = 0; i < 16; ++i) {
            x[i] = static_cast<uint32_t>(block[4 * i]) |
                   (static_cast<uint32_t>(block[4 * i + 1]) << 8) |
                   (static_cast<uint32_t>(block[4 * i + 2]) << 16) |
                   (static_cast<uint32_t>(block[4 * i + 3]) << 24);
        }
        uint32_t al = h0, bl = h1, cl = h2, dl = h3, el = h4;
        uint32_t ar = h0, br = h1, cr = h2, dr = h3, er = h4;
        for (uint32_t j = 0; j < 80; ++j) {
            uint32_t t = rol(al + ripemdF(j, bl, cl, dl) + x[r[j]] + ripemdK(j), s[j]) + el;
            al = el; el = dl; dl = rol(cl, 10); cl = bl; bl = t;
            t = rol(ar + ripemdF(79 - j, br, cr, dr) + x[rp[j]] + ripemdKp(j), sp[j]) + er;
            ar = er; er = dr; dr = rol(cr, 10); cr = br; br = t;
        }
        uint32_t t = h1 + cl + dr;
        h1 = h2 + dl + er; h2 = h3 + el + ar; h3 = h4 + al + br; h4 = h0 + bl + cr; h0 = t;
    }
    const uint32_t h[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[4 * i] = static_cast<uint8_t>(h[i]);
        out[4 * i + 1] = static_cast<uint8_t>(h[i] >> 8);
        out[4 * i + 2] = static_cast<uint8_t>(h[i] >> 16);
        out[4 * i + 3] = static_cast<uint8_t>(h[i] >> 24);
    }
}

void putU32(uint8_t* out, uint32_t value) {
    crypto::putU32BE(out, value);
}

int base58Value(char c) {
    for (int i = 0; XRPL_ALPHABET[i]; ++i) if (XRPL_ALPHABET[i] == c) return i;
    return -1;
}

bool base58Encode(const uint8_t* data, size_t len, char* out, size_t outSize) {
    uint8_t digits[96] = {};
    size_t digitLen = 1, zeros = 0;
    while (zeros < len && data[zeros] == 0) ++zeros;
    for (size_t i = zeros; i < len; ++i) {
        uint32_t carry = data[i];
        for (size_t j = 0; j < digitLen; ++j) {
            carry += static_cast<uint32_t>(digits[j]) << 8;
            digits[j] = carry % 58; carry /= 58;
        }
        while (carry) {
            if (digitLen >= sizeof(digits)) return false;
            digits[digitLen++] = carry % 58; carry /= 58;
        }
    }
    const size_t total = zeros + digitLen;
    if (total + 1 > outSize) return false;
    size_t pos = 0;
    for (; pos < zeros; ++pos) out[pos] = XRPL_ALPHABET[0];
    for (size_t i = 0; i < digitLen; ++i) out[pos + i] = XRPL_ALPHABET[digits[digitLen - 1 - i]];
    out[total] = '\0';
    return true;
}

bool base58Decode(const char* text, uint8_t* out, size_t outSize, size_t* outLen) {
    uint8_t bytes[96] = {};
    size_t byteLen = 1, zeros = 0;
    while (text[zeros] == XRPL_ALPHABET[0]) ++zeros;
    for (const char* p = text + zeros; *p; ++p) {
        int value = base58Value(*p);
        if (value < 0) return false;
        uint32_t carry = value;
        for (size_t j = 0; j < byteLen; ++j) {
            carry += static_cast<uint32_t>(bytes[j]) * 58;
            bytes[j] = carry & 0xFF;
            carry >>= 8;
        }
        while (carry) {
            if (byteLen >= sizeof(bytes)) return false;
            bytes[byteLen++] = carry & 0xFF;
            carry >>= 8;
        }
    }
    const size_t total = zeros + byteLen;
    if (total > outSize) return false;
    memset(out, 0, zeros);
    for (size_t i = 0; i < byteLen; ++i) out[zeros + i] = bytes[byteLen - 1 - i];
    if (outLen) *outLen = total;
    return true;
}

bool base58Check(uint8_t version, const uint8_t* payload, size_t payloadLen, char* out, size_t outSize) {
    uint8_t data[1 + 64 + 4];
    data[0] = version;
    memcpy(data + 1, payload, payloadLen);
    uint8_t a[32], b[32];
    crypto::sha256(data, 1 + payloadLen, a);
    crypto::sha256(a, sizeof(a), b);
    memcpy(data + 1 + payloadLen, b, 4);
    return base58Encode(data, 1 + payloadLen + 4, out, outSize);
}

bool base58CheckDecode(uint8_t expectedVersion, const char* text, uint8_t* payload, size_t payloadLen) {
    uint8_t data[96];
    size_t len = 0;
    if (!base58Decode(text, data, sizeof(data), &len) || len != payloadLen + 5) return false;
    if (data[0] != expectedVersion) return false;
    uint8_t a[32], b[32];
    crypto::sha256(data, payloadLen + 1, a);
    crypto::sha256(a, sizeof(a), b);
    if (memcmp(data + payloadLen + 1, b, 4) != 0) return false;
    memcpy(payload, data + 1, payloadLen);
    return true;
}

// KEK = PBKDF2-HMAC-SHA256( deviceMAC || PIN, salt, iters ). Binding the MAC
// keeps the seal device-specific; the PIN supplies the actual secret entropy.
bool deriveStorageKey(const char* pin, const uint8_t salt[SALT_LEN], uint8_t out[32]) {
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    size_t pinLen = pin ? strlen(pin) : 0;
    if (pinLen > 32) pinLen = 32;
    uint8_t password[6 + 32];
    memcpy(password, mac, sizeof(mac));
    if (pinLen) memcpy(password + sizeof(mac), pin, pinLen);
    const size_t pwLen = sizeof(mac) + pinLen;

    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool ok = info != nullptr && mbedtls_md_setup(&md, info, 1) == 0 &&
              mbedtls_pkcs5_pbkdf2_hmac(&md, password, pwLen, salt, SALT_LEN, PBKDF2_ITERS, 32,
                                        out) == 0;
    mbedtls_md_free(&md);
    memset(password, 0, sizeof(password));
    return ok;
}

bool gcmEncrypt(const uint8_t seed[SEED_LEN], const uint8_t key[32], uint8_t blob[BLOB_LEN]) {
    if (!fillEntropy(blob, NONCE_LEN, "seed-gcm-nonce")) return false;
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
              mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, SEED_LEN, blob, NONCE_LEN,
                                         reinterpret_cast<const uint8_t*>(STORAGE_AAD), strlen(STORAGE_AAD),
                                         seed, blob + NONCE_LEN, TAG_LEN,
                                         blob + NONCE_LEN + SEED_LEN) == 0;
    mbedtls_gcm_free(&gcm);
    return ok;
}

bool gcmDecrypt(const uint8_t blob[BLOB_LEN], const uint8_t key[32], uint8_t seed[SEED_LEN]) {
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
              mbedtls_gcm_auth_decrypt(&gcm, SEED_LEN, blob, NONCE_LEN,
                                        reinterpret_cast<const uint8_t*>(STORAGE_AAD), strlen(STORAGE_AAD),
                                        blob + NONCE_LEN + SEED_LEN, TAG_LEN,
                                        blob + NONCE_LEN, seed) == 0;
    mbedtls_gcm_free(&gcm);
    if (!ok) memset(seed, 0, SEED_LEN);
    return ok;
}

bool validPrivate(mbedtls_ecp_group& grp, const uint8_t priv[32]) {
    mbedtls_mpi d;
    mbedtls_mpi_init(&d);
    bool ok = mbedtls_mpi_read_binary(&d, priv, 32) == 0 && mbedtls_mpi_cmp_int(&d, 0) > 0 &&
              mbedtls_mpi_cmp_mpi(&d, &grp.N) < 0;
    mbedtls_mpi_free(&d);
    return ok;
}

bool compressedPublicKey(mbedtls_ecp_group& grp, const uint8_t priv[32], uint8_t out[33]) {
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);
    bool ok = mbedtls_mpi_read_binary(&d, priv, 32) == 0 &&
              mbedtls_ecp_mul(&grp, &q, &d, &grp.G, espRng, nullptr) == 0 &&
              mbedtls_mpi_write_binary(&q.X, out + 1, 32) == 0;
    if (ok) out[0] = mbedtls_mpi_get_bit(&q.Y, 0) ? 0x03 : 0x02;
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    return ok;
}

bool deriveRootPrivate(mbedtls_ecp_group& grp, const uint8_t seed[SEED_LEN], uint8_t out[32]) {
    uint8_t input[20];
    memcpy(input, seed, SEED_LEN);
    for (uint32_t i = 0; i < 0xFFFFFFFF; ++i) {
        putU32(input + 16, i);
        crypto::sha512Half(input, sizeof(input), out);
        if (validPrivate(grp, out)) {
            memset(input, 0, sizeof(input));
            return true;
        }
    }
    memset(input, 0, sizeof(input));
    return false;
}

bool deriveAccountPrivate(mbedtls_ecp_group& grp, const uint8_t root[32], const uint8_t publicGen[33],
                          uint8_t out[32]) {
    uint8_t input[41], secret[32];
    memcpy(input, publicGen, 33);
    putU32(input + 33, 0);
    mbedtls_mpi rootMpi, secretMpi, result;
    mbedtls_mpi_init(&rootMpi); mbedtls_mpi_init(&secretMpi); mbedtls_mpi_init(&result);
    bool rootOk = mbedtls_mpi_read_binary(&rootMpi, root, 32) == 0;
    bool ok = false;
    for (uint32_t i = 0; rootOk && i < 0xFFFFFFFF; ++i) {
        putU32(input + 37, i);
        crypto::sha512Half(input, sizeof(input), secret);
        if (!validPrivate(grp, secret)) continue;
        if (mbedtls_mpi_read_binary(&secretMpi, secret, 32) != 0) continue;
        if (mbedtls_mpi_add_mpi(&result, &secretMpi, &rootMpi) != 0) continue;
        if (mbedtls_mpi_mod_mpi(&result, &result, &grp.N) != 0 || mbedtls_mpi_cmp_int(&result, 0) == 0) continue;
        ok = mbedtls_mpi_write_binary(&result, out, 32) == 0;
        break;
    }
    mbedtls_mpi_free(&result); mbedtls_mpi_free(&secretMpi); mbedtls_mpi_free(&rootMpi);
    memset(secret, 0, sizeof(secret));
    memset(input, 0, sizeof(input));
    return ok;
}

bool deriveAccountPrivate(uint8_t accountPriv[32], uint8_t accountPub[33]) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_group_init(&grp);
    bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1) == 0;
    uint8_t root[32], publicGen[33];
    ok = ok && deriveRootPrivate(grp, g_seed, root);
    ok = ok && compressedPublicKey(grp, root, publicGen);
    ok = ok && deriveAccountPrivate(grp, root, publicGen, accountPriv);
    ok = ok && compressedPublicKey(grp, accountPriv, accountPub);
    memset(root, 0, sizeof(root));
    mbedtls_ecp_group_free(&grp);
    return ok;
}

bool signSha512HalfImpl(const uint8_t* data, size_t len, uint8_t* der, size_t* derLen) {
    if (!g_unlocked || !der || !derLen) return false;

    uint8_t accountPriv[32], accountPub[33], hash[32];
    if (!deriveAccountPrivate(accountPriv, accountPub)) return false;
    crypto::sha512Half(data, len, hash);

    static mbedtls_ecp_group grp;
    static mbedtls_mpi r, s;
    static bool wsReady = false;
    if (!wsReady) {
        mbedtls_ecp_group_init(&grp);
        mbedtls_mpi_init(&r);
        mbedtls_mpi_init(&s);
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1) != 0) return false;
        wsReady = true;
    }

    const bool ok = ecdsaSignDigest(&grp, accountPriv, hash, &r, &s) &&
                    derEncodeEcdsa(&r, &s, der, derLen);
    memset(accountPriv, 0, sizeof(accountPriv));
    return ok;
}

bool sealSeed(const char* pin) {
    uint8_t key[32];
    if (!deriveStorageKey(pin, g_salt, key)) return false;
    uint8_t blob[BLOB_LEN];
    bool ok = gcmEncrypt(g_seed, key, blob);
    memset(key, 0, sizeof(key));
    if (ok) {
        prefs.begin(PREF_NS, false);
        size_t w1 = prefs.putBytes(PREF_BLOB, blob, sizeof(blob));
        size_t w2 = prefs.putBytes(PREF_SALT, g_salt, SALT_LEN);
        prefs.putInt(PREF_FAILS, 0);
        prefs.end();
        ok = (w1 == BLOB_LEN) && (w2 == SALT_LEN);
    }
    memset(blob, 0, sizeof(blob));
    return ok;
}

bool rebuildDerived() {
    uint8_t accountPriv[32], accountPub[33], pubHash[32], accountId[20];
    bool ok = deriveAccountPrivate(accountPriv, accountPub);
    if (ok) {
        crypto::sha256(accountPub, sizeof(accountPub), pubHash);
        ripemd160(pubHash, sizeof(pubHash), accountId);
        ok = base58Check(0x00, accountId, sizeof(accountId), g_address, sizeof(g_address));
        crypto::bytesToHex(accountPub, sizeof(accountPub), g_publicKeyHex, sizeof(g_publicKeyHex));
    }
    memset(accountPriv, 0, sizeof(accountPriv));
    return ok;
}

}  // namespace

bool signSha512Half(const uint8_t* data, size_t len, uint8_t* der, size_t* derLen) {
    return signSha512HalfImpl(data, len, der, derLen);
}

void beginEntropy() {
    beginEntropyInternal();
}

void begin() {
    prefs.begin(PREF_NS, true);
    const bool haveBlob = prefs.getBytesLength(PREF_BLOB) == BLOB_LEN;
    const bool haveSalt = prefs.getBytesLength(PREF_SALT) == SALT_LEN;
    if (haveBlob && haveSalt) {
        prefs.getBytes(PREF_SALT, g_salt, SALT_LEN);
        g_fails = prefs.getInt(PREF_FAILS, 0);
        g_hasWallet = true;  // present but still sealed; unlock() needs the PIN
    }
    prefs.end();
    g_unlocked = false;
}

bool hasWallet() { return g_hasWallet; }
bool isUnlocked() { return g_unlocked; }
int failedAttempts() { return g_fails; }

void lock() {
    memset(g_seed, 0, sizeof(g_seed));
    strcpy(g_address, "NO WALLET");
    g_publicKeyHex[0] = '\0';
    g_unlocked = false;
}

bool unlock(const char* pin) {
    if (!g_hasWallet) return false;
    uint8_t blob[BLOB_LEN], key[32];
    prefs.begin(PREF_NS, true);
    const bool got = prefs.getBytes(PREF_BLOB, blob, sizeof(blob)) == BLOB_LEN;
    prefs.end();
    if (!got) {
        memset(blob, 0, sizeof(blob));
        return false;
    }

    const bool ok = deriveStorageKey(pin, g_salt, key) && gcmDecrypt(blob, key, g_seed) &&
                    rebuildDerived();
    memset(key, 0, sizeof(key));
    memset(blob, 0, sizeof(blob));

    if (ok) {
        g_unlocked = true;
        g_fails = 0;
        prefs.begin(PREF_NS, false);
        prefs.putInt(PREF_FAILS, 0);
        prefs.end();
    } else {
        lock();
        ++g_fails;
        prefs.begin(PREF_NS, false);
        prefs.putInt(PREF_FAILS, g_fails);
        prefs.end();
        if (g_fails >= cfg::MAX_PIN_FAILS) clearWallet();  // anti-brute wipe
    }
    return ok;
}

bool createWallet(const char* pin) {
    if (!fillEntropy(g_salt, sizeof(g_salt), "wallet-salt") ||
        !fillEntropy(g_seed, sizeof(g_seed), "wallet-seed")) {
        return false;
    }
    const bool ok = rebuildDerived() && sealSeed(pin);
    if (ok) {
        g_hasWallet = true;
        g_unlocked = true;
        g_fails = 0;
    } else {
        clearWallet();
    }
    return ok;
}

bool importSeed(const char* seed, const char* pin) {
    uint8_t decoded[SEED_LEN];
    if (!base58CheckDecode(0x21, seed, decoded, sizeof(decoded))) return false;
    if (!fillEntropy(g_salt, sizeof(g_salt), "wallet-import-salt")) {
        memset(decoded, 0, sizeof(decoded));
        return false;
    }
    memcpy(g_seed, decoded, sizeof(g_seed));
    memset(decoded, 0, sizeof(decoded));
    const bool ok = rebuildDerived() && sealSeed(pin);
    if (ok) {
        g_hasWallet = true;
        g_unlocked = true;
        g_fails = 0;
    } else {
        clearWallet();
    }
    return ok;
}

bool changePin(const char* newPin) {
    if (!g_unlocked) return false;
    if (!fillEntropy(g_salt, sizeof(g_salt), "wallet-pin-salt")) return false;
    return sealSeed(newPin);
}

bool clearWallet() {
    prefs.begin(PREF_NS, false);
    prefs.remove(PREF_BLOB);
    prefs.remove(PREF_SALT);
    prefs.remove(PREF_FAILS);
    prefs.end();
    memset(g_seed, 0, sizeof(g_seed));
    memset(g_salt, 0, sizeof(g_salt));
    strcpy(g_address, "NO WALLET");
    g_publicKeyHex[0] = '\0';
    g_hasWallet = false;
    g_unlocked = false;
    g_fails = 0;
    return true;
}

const char* address() { return g_unlocked ? g_address : "LOCKED"; }
const char* publicKeyHex() { return g_unlocked ? g_publicKeyHex : ""; }

bool exportFamilySeed(char* out, size_t outSize) {
    if (outSize) out[0] = '\0';
    if (!g_unlocked) return false;
    return base58Check(0x21, g_seed, sizeof(g_seed), out, outSize);
}

bool accountIdFromAddress(const char* address, uint8_t out[20]) {
    return base58CheckDecode(0x00, address, out, 20);
}

bool diagnosticEntropySample(char* outHex, size_t outSize) {
    if (!cfg::ENTROPY_DIAGNOSTICS || !outHex || outSize < 65) return false;
    uint8_t sample[32];
    const bool ok = fillEntropy(sample, sizeof(sample), "entropy-diagnostic");
    if (ok) crypto::bytesToHex(sample, sizeof(sample), outHex, outSize);
    memset(sample, 0, sizeof(sample));
    return ok;
}

void shortenAddress(const char* full, char* out, size_t outSize) {
    if (outSize == 0) return;
    const size_t len = strlen(full);
    if (len <= 18) {
        strncpy(out, full, outSize - 1);
        out[outSize - 1] = '\0';
        return;
    }
    snprintf(out, outSize, "%.8s...%s", full, full + len - 4);
}

}  // namespace wallet
