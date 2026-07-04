#include "tx_signer.h"

#include <ArduinoJson.h>

#include <cstring>

#include "crypto_util.h"
#include "wallet.h"

namespace txsigner {
namespace {

// Typical mainnet fees are 10–20 drops; warn only when the fee is genuinely
// unusual (5000 drops = 0.005 XRP) instead of flagging routine transactions.
constexpr uint32_t HIGH_FEE_DROPS = 5000;
constexpr uint8_t STX_PREFIX[] = {0x53, 0x54, 0x58, 0x00};
constexpr uint8_t TXN_PREFIX[] = {0x54, 0x58, 0x4E, 0x00};

// XRPL serialized-amount flags (8-byte amount field).
constexpr uint64_t AMT_NOT_XRP = 0x8000000000000000ULL;
constexpr uint64_t AMT_POSITIVE = 0x4000000000000000ULL;

struct Bytes {
    uint8_t data[2048];
    size_t len = 0;
};

bool append(Bytes& b, uint8_t v) {
    if (b.len >= sizeof(b.data)) return false;
    b.data[b.len++] = v;
    return true;
}

bool append(Bytes& b, const uint8_t* data, size_t len) {
    if (b.len + len > sizeof(b.data)) return false;
    memcpy(b.data + b.len, data, len);
    b.len += len;
    return true;
}

bool appendHex(Bytes& b, const char* hex) {
    const size_t n = strlen(hex);
    if (n % 2 != 0) return false;
    for (size_t i = 0; i < n; i += 2) {
        const int hi = crypto::hexNibble(hex[i]);
        const int lo = crypto::hexNibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        if (!append(b, static_cast<uint8_t>((hi << 4) | lo))) return false;
    }
    return true;
}

bool hexToBytes(const char* hex, uint8_t* out, size_t outLen) {
    return crypto::hexToBytes(hex, out, outLen);
}

bool putU16(Bytes& b, uint16_t v) {
    return append(b, static_cast<uint8_t>(v >> 8)) && append(b, static_cast<uint8_t>(v));
}

bool putU32(Bytes& b, uint32_t v) {
    return append(b, static_cast<uint8_t>(v >> 24)) && append(b, static_cast<uint8_t>(v >> 16)) &&
           append(b, static_cast<uint8_t>(v >> 8)) && append(b, static_cast<uint8_t>(v));
}

template <typename V>
uint32_t readU32(V v) {
    return v.template is<uint32_t>() ? v.template as<uint32_t>() : 0;
}

bool putVl(Bytes& b, const uint8_t* data, size_t len) {
    if (len > 192) return false;
    return append(b, static_cast<uint8_t>(len)) && append(b, data, len);
}

bool putXrpAmount(Bytes& b, const char* drops) {
    char* end = nullptr;
    uint64_t value = strtoull(drops, &end, 10);
    if (!drops[0] || (end && *end != '\0') || value > 0x3FFFFFFFFFFFFFFFULL) return false;
    value |= 0x4000000000000000ULL;  // XRP amount marker (not-XRP bit clear)
    for (int i = 7; i >= 0; --i) {
        if (!append(b, static_cast<uint8_t>((value >> (8 * i)) & 0xFF))) return false;
    }
    return true;
}

// Parse a decimal string into a normalized (mantissa, base-10 exponent, sign).
// Rejects values needing more than 16 significant digits (XRPL mantissa limit).
bool parseDecimal(const char* s, uint64_t* mantOut, int* expOut, bool* negOut) {
    *negOut = false;
    const char* p = s;
    if (*p == '-') {
        *negOut = true;
        ++p;
    } else if (*p == '+') {
        ++p;
    }
    char digits[40];
    int nd = 0, exp = 0;
    bool seenDot = false, any = false;
    for (; *p; ++p) {
        if (*p == '.') {
            if (seenDot) return false;
            seenDot = true;
            continue;
        }
        if (*p < '0' || *p > '9') return false;
        any = true;
        if (seenDot) --exp;
        if (nd < 39) {
            digits[nd++] = *p;
        } else {
            return false;
        }
    }
    if (!any) return false;
    int start = 0;
    while (start < nd - 1 && digits[start] == '0') ++start;
    int end = nd;
    while (end - 1 > start && digits[end - 1] == '0') {  // trailing zeros -> exponent
        --end;
        ++exp;
    }
    const int sig = end - start;
    if (sig == 1 && digits[start] == '0') {
        *mantOut = 0;
        *expOut = 0;
        return true;
    }
    if (sig > 16) return false;
    uint64_t m = 0;
    for (int i = start; i < end; ++i) m = m * 10 + static_cast<uint64_t>(digits[i] - '0');
    *mantOut = m;
    *expOut = exp;
    return true;
}

// Fill a 160-bit (20-byte) currency code: 3-char ISO codes go at offset 12,
// 40-hex strings are taken verbatim. "XRP" is invalid as an IOU currency.
bool fillCurrency(const char* code, uint8_t out[20]) {
    memset(out, 0, 20);
    const size_t n = strlen(code);
    if (n == 3) {
        if (strcmp(code, "XRP") == 0) return false;
        out[12] = static_cast<uint8_t>(code[0]);
        out[13] = static_cast<uint8_t>(code[1]);
        out[14] = static_cast<uint8_t>(code[2]);
        return true;
    }
    if (n == 40) return hexToBytes(code, out, 20);
    return false;
}

// Serialize an IOU amount field: 8-byte value + 20-byte currency + 20-byte issuer.
bool putIouAmount(Bytes& b, const char* currency, const char* issuer, const char* value) {
    uint8_t curr[20], acct[20];
    if (!fillCurrency(currency, curr)) return false;
    if (!wallet::accountIdFromAddress(issuer, acct)) return false;

    uint64_t mant = 0;
    int exp = 0;
    bool neg = false;
    if (!parseDecimal(value, &mant, &exp, &neg)) return false;

    uint64_t amount;
    if (mant == 0) {
        amount = AMT_NOT_XRP;  // canonical zero
    } else {
        if (neg) return false;  // trust limits are non-negative
        while (mant < 1000000000000000ULL) {  // normalize to [1e15, 1e16)
            mant *= 10;
            --exp;
        }
        while (mant > 9999999999999999ULL) {
            mant /= 10;
            ++exp;
        }
        if (exp < -96 || exp > 80) return false;
        amount = AMT_NOT_XRP | AMT_POSITIVE |
                 (static_cast<uint64_t>(exp + 97) << 54) | mant;
    }
    for (int i = 7; i >= 0; --i) {
        if (!append(b, static_cast<uint8_t>((amount >> (8 * i)) & 0xFF))) return false;
    }
    return append(b, curr, sizeof(curr)) && append(b, acct, sizeof(acct));
}

// Serialize an Amount field value (header byte already emitted by the caller):
// a JSON string is XRP drops, a JSON object {currency,issuer,value} is an IOU.
template <typename V>
bool putAmountValue(Bytes& b, V v) {
    if (v.template is<const char*>()) return putXrpAmount(b, v.template as<const char*>());
    if (v.template is<JsonObjectConst>()) {
        JsonObjectConst o = v.template as<JsonObjectConst>();
        return putIouAmount(b, o["currency"] | "", o["issuer"] | "", o["value"] | "");
    }
    return false;
}

// Serialize an STIssue (type 24) value: 20-byte currency, plus a 20-byte issuer
// when the currency is not XRP. {"currency":"XRP"} is the canonical 20 zero bytes.
template <typename V>
bool putIssue(Bytes& b, V v) {
    if (!v.template is<JsonObjectConst>()) return false;
    JsonObjectConst o = v.template as<JsonObjectConst>();
    const char* currency = o["currency"] | "";
    if (!currency[0]) return false;
    uint8_t curr[20];
    if (strcmp(currency, "XRP") == 0) {
        memset(curr, 0, sizeof(curr));
        return append(b, curr, sizeof(curr));
    }
    if (!fillCurrency(currency, curr)) return false;
    if (!append(b, curr, sizeof(curr))) return false;
    uint8_t acct[20];
    if (!wallet::accountIdFromAddress(o["issuer"] | "", acct)) return false;
    return append(b, acct, sizeof(acct));
}

// Serialize a PathSet (type 18): paths joined by 0xFF, terminated by 0x00. Each
// step is a type byte (account 0x01 | currency 0x10 | issuer 0x20) followed by
// the present 20-byte fields in that order.
template <typename V>
bool putPathSet(Bytes& b, V v) {
    if (!v.template is<JsonArrayConst>()) return false;
    bool firstPath = true;
    for (JsonVariantConst pv : v.template as<JsonArrayConst>()) {
        if (!pv.is<JsonArrayConst>()) return false;
        if (!firstPath && !append(b, 0xFF)) return false;
        firstPath = false;
        for (JsonVariantConst sv : pv.as<JsonArrayConst>()) {
            if (!sv.is<JsonObjectConst>()) return false;
            JsonObjectConst step = sv.as<JsonObjectConst>();
            const bool hasAcct = step["account"].is<const char*>();
            const bool hasCurr = step["currency"].is<const char*>();
            const bool hasIss = step["issuer"].is<const char*>();
            const uint8_t stepType =
                (hasAcct ? 0x01 : 0) | (hasCurr ? 0x10 : 0) | (hasIss ? 0x20 : 0);
            if (stepType == 0 || !append(b, stepType)) return false;
            if (hasAcct) {
                uint8_t a[20];
                if (!wallet::accountIdFromAddress(step["account"] | "", a)) return false;
                if (!append(b, a, sizeof(a))) return false;
            }
            if (hasCurr) {
                uint8_t c[20];
                const char* cur = step["currency"] | "";
                if (strcmp(cur, "XRP") == 0) {
                    memset(c, 0, sizeof(c));
                } else if (!fillCurrency(cur, c)) {
                    return false;
                }
                if (!append(b, c, sizeof(c))) return false;
            }
            if (hasIss) {
                uint8_t i[20];
                if (!wallet::accountIdFromAddress(step["issuer"] | "", i)) return false;
                if (!append(b, i, sizeof(i))) return false;
            }
        }
    }
    return append(b, 0x00);
}

// Human-readable amount for the review screen: "<drops> drops" or "<value> <ccy>".
template <typename V>
void formatAmount(V v, char* out, size_t n) {
    if (v.template is<const char*>()) {
        snprintf(out, n, "%s drops", v.template as<const char*>());
    } else if (v.template is<JsonObjectConst>()) {
        JsonObjectConst o = v.template as<JsonObjectConst>();
        snprintf(out, n, "%s %s", o["value"] | "?", o["currency"] | "?");
    } else {
        snprintf(out, n, "-");
    }
}

// Short currency label for an STIssue object (used for AMM pool pairs).
template <typename V>
void formatIssue(V v, char* out, size_t n) {
    if (v.template is<JsonObjectConst>()) {
        JsonObjectConst o = v.template as<JsonObjectConst>();
        snprintf(out, n, "%s", o["currency"] | "?");
    } else {
        snprintf(out, n, "?");
    }
}

bool resolveSigner(JsonObject tx, uint8_t account[20], uint8_t pubkey[33]) {
    if (!wallet::accountIdFromAddress(tx["Account"] | "", account)) return false;
    Bytes pub;
    if (!appendHex(pub, wallet::publicKeyHex()) || pub.len != 33) return false;
    memcpy(pubkey, pub.data, 33);
    return true;
}

// Common trailing fields: SigningPubKey (7/3), optional TxnSignature (7/4),
// Account (8/1). Callers append any AccountID field > Account afterwards.
bool appendPubAndAccount(Bytes& b, const uint8_t pubkey[33], const uint8_t account[20],
                         bool includeSig, const uint8_t* sig, size_t sigLen) {
    if (!append(b, 0x73) || !putVl(b, pubkey, 33)) return false;
    if (includeSig && (!append(b, 0x74) || !putVl(b, sig, sigLen))) return false;
    if (!append(b, 0x81) || !putVl(b, account, 20)) return false;
    return true;
}

// Emit the leading UInt32 block shared by all types (canonical order:
// Flags(2) < SourceTag(3) < Sequence(4)). LastLedger/type-specific fields follow.
bool appendCommonUInts(Bytes& b, JsonObject tx) {
    const uint32_t flags = readU32(tx["Flags"]);
    const uint32_t sourceTag = readU32(tx["SourceTag"]);
    const uint32_t sequence = readU32(tx["Sequence"]);
    if (!append(b, 0x22) || !putU32(b, flags)) return false;
    if (tx["SourceTag"].is<uint32_t>() && (!append(b, 0x23) || !putU32(b, sourceTag))) return false;
    if (!append(b, 0x24) || !putU32(b, sequence)) return false;
    return true;
}

bool appendPayment(Bytes& b, JsonObject tx, bool includeSig, const uint8_t* sig, size_t sigLen) {
    uint8_t account[20], destination[20], pubkey[33];
    if (!resolveSigner(tx, account, pubkey)) return false;
    if (!wallet::accountIdFromAddress(tx["Destination"] | "", destination)) return false;
    const uint32_t lastLedger = readU32(tx["LastLedgerSequence"]);
    const uint32_t destinationTag = readU32(tx["DestinationTag"]);

    // Canonical field order (ascending type code, then field code).
    if (!append(b, 0x12) || !putU16(b, 0)) return false;  // Payment
    if (!appendCommonUInts(b, tx)) return false;
    if (tx["DestinationTag"].is<uint32_t>() &&
        (!append(b, 0x2E) || !putU32(b, destinationTag)))
        return false;
    if (!append(b, 0x20) || !append(b, 0x1B) || !putU32(b, lastLedger)) return false;
    if (!append(b, 0x61) || !putAmountValue(b, tx["Amount"])) return false;  // Amount (6/1)
    if (!append(b, 0x68) || !putXrpAmount(b, tx["Fee"] | "")) return false;  // Fee (6/8)
    if (!tx["SendMax"].isNull() &&
        (!append(b, 0x69) || !putAmountValue(b, tx["SendMax"])))  // SendMax (6/9)
        return false;
    if (!tx["DeliverMin"].isNull() &&
        (!append(b, 0x6A) || !putAmountValue(b, tx["DeliverMin"])))  // DeliverMin (6/10)
        return false;
    if (!appendPubAndAccount(b, pubkey, account, includeSig, sig, sigLen)) return false;
    if (!append(b, 0x83) || !putVl(b, destination, 20)) return false;
    if (tx["Paths"].is<JsonArray>() &&
        (!append(b, 0x01) || !append(b, 0x12) || !putPathSet(b, tx["Paths"])))  // Paths (18/1)
        return false;
    return true;
}

bool appendTrustSet(Bytes& b, JsonObject tx, bool includeSig, const uint8_t* sig, size_t sigLen) {
    uint8_t account[20], pubkey[33];
    if (!resolveSigner(tx, account, pubkey)) return false;
    JsonObject limit = tx["LimitAmount"].as<JsonObject>();
    const uint32_t lastLedger = readU32(tx["LastLedgerSequence"]);

    if (!append(b, 0x12) || !putU16(b, 20)) return false;  // TrustSet
    if (!appendCommonUInts(b, tx)) return false;
    if (!append(b, 0x20) || !append(b, 0x1B) || !putU32(b, lastLedger)) return false;
    if (!append(b, 0x63) ||
        !putIouAmount(b, limit["currency"] | "", limit["issuer"] | "", limit["value"] | ""))
        return false;
    if (!append(b, 0x68) || !putXrpAmount(b, tx["Fee"] | "")) return false;
    if (!appendPubAndAccount(b, pubkey, account, includeSig, sig, sigLen)) return false;
    return true;
}

bool appendOfferCancel(Bytes& b, JsonObject tx, bool includeSig, const uint8_t* sig,
                       size_t sigLen) {
    uint8_t account[20], pubkey[33];
    if (!resolveSigner(tx, account, pubkey)) return false;
    const uint32_t lastLedger = readU32(tx["LastLedgerSequence"]);
    const uint32_t offerSeq = readU32(tx["OfferSequence"]);

    if (!append(b, 0x12) || !putU16(b, 8)) return false;  // OfferCancel
    if (!appendCommonUInts(b, tx)) return false;
    if (!append(b, 0x20) || !append(b, 0x19) || !putU32(b, offerSeq)) return false;  // OfferSequence(25)
    if (!append(b, 0x20) || !append(b, 0x1B) || !putU32(b, lastLedger)) return false;
    if (!append(b, 0x68) || !putXrpAmount(b, tx["Fee"] | "")) return false;
    if (!appendPubAndAccount(b, pubkey, account, includeSig, sig, sigLen)) return false;
    return true;
}

bool appendAccountSet(Bytes& b, JsonObject tx, bool includeSig, const uint8_t* sig, size_t sigLen) {
    uint8_t account[20], pubkey[33];
    if (!resolveSigner(tx, account, pubkey)) return false;
    const uint32_t lastLedger = readU32(tx["LastLedgerSequence"]);

    if (!append(b, 0x12) || !putU16(b, 3)) return false;  // AccountSet
    if (!appendCommonUInts(b, tx)) return false;
    if (!append(b, 0x20) || !append(b, 0x1B) || !putU32(b, lastLedger)) return false;
    if (tx["SetFlag"].is<uint32_t>() &&
        (!append(b, 0x20) || !append(b, 0x21) || !putU32(b, readU32(tx["SetFlag"]))))  // SetFlag(33)
        return false;
    if (tx["ClearFlag"].is<uint32_t>() &&
        (!append(b, 0x20) || !append(b, 0x22) || !putU32(b, readU32(tx["ClearFlag"]))))  // ClearFlag(34)
        return false;
    if (!append(b, 0x68) || !putXrpAmount(b, tx["Fee"] | "")) return false;
    if (!appendPubAndAccount(b, pubkey, account, includeSig, sig, sigLen)) return false;
    return true;
}

// OfferCreate (DEX / meme-coin buy/sell, also market swaps via tfImmediateOrCancel
// or tfFillOrKill). TakerPays = what you receive, TakerGets = what you give.
bool appendOfferCreate(Bytes& b, JsonObject tx, bool includeSig, const uint8_t* sig,
                       size_t sigLen) {
    uint8_t account[20], pubkey[33];
    if (!resolveSigner(tx, account, pubkey)) return false;
    const uint32_t lastLedger = readU32(tx["LastLedgerSequence"]);

    if (!append(b, 0x12) || !putU16(b, 7)) return false;  // OfferCreate
    if (!appendCommonUInts(b, tx)) return false;
    if (tx["Expiration"].is<uint32_t>() &&
        (!append(b, 0x2A) || !putU32(b, readU32(tx["Expiration"]))))  // Expiration (2/10)
        return false;
    if (tx["OfferSequence"].is<uint32_t>() &&
        (!append(b, 0x20) || !append(b, 0x19) || !putU32(b, readU32(tx["OfferSequence"]))))  // (2/25)
        return false;
    if (!append(b, 0x20) || !append(b, 0x1B) || !putU32(b, lastLedger)) return false;  // LastLedger
    if (!append(b, 0x64) || !putAmountValue(b, tx["TakerPays"])) return false;  // TakerPays (6/4)
    if (!append(b, 0x65) || !putAmountValue(b, tx["TakerGets"])) return false;  // TakerGets (6/5)
    if (!append(b, 0x68) || !putXrpAmount(b, tx["Fee"] | "")) return false;     // Fee (6/8)
    if (!appendPubAndAccount(b, pubkey, account, includeSig, sig, sigLen)) return false;
    return true;
}

// AMMDeposit (add liquidity). Asset/Asset2 identify the pool; the optional
// Amount/Amount2/LPTokenOut/EPrice select the deposit mode.
bool appendAmmDeposit(Bytes& b, JsonObject tx, bool includeSig, const uint8_t* sig,
                      size_t sigLen) {
    uint8_t account[20], pubkey[33];
    if (!resolveSigner(tx, account, pubkey)) return false;
    const uint32_t lastLedger = readU32(tx["LastLedgerSequence"]);

    if (!append(b, 0x12) || !putU16(b, 36)) return false;  // AMMDeposit
    if (!appendCommonUInts(b, tx)) return false;
    if (!append(b, 0x20) || !append(b, 0x1B) || !putU32(b, lastLedger)) return false;
    if (!tx["Amount"].isNull() &&
        (!append(b, 0x61) || !putAmountValue(b, tx["Amount"])))  // Amount (6/1)
        return false;
    if (!append(b, 0x68) || !putXrpAmount(b, tx["Fee"] | "")) return false;  // Fee (6/8)
    if (!tx["Amount2"].isNull() &&
        (!append(b, 0x6B) || !putAmountValue(b, tx["Amount2"])))  // Amount2 (6/11)
        return false;
    if (!tx["LPTokenOut"].isNull() &&
        (!append(b, 0x60) || !append(b, 0x19) || !putAmountValue(b, tx["LPTokenOut"])))  // (6/25)
        return false;
    if (!tx["EPrice"].isNull() &&
        (!append(b, 0x60) || !append(b, 0x1B) || !putAmountValue(b, tx["EPrice"])))  // (6/27)
        return false;
    if (!appendPubAndAccount(b, pubkey, account, includeSig, sig, sigLen)) return false;
    if (!append(b, 0x03) || !append(b, 0x18) || !putIssue(b, tx["Asset"])) return false;   // (24/3)
    if (!append(b, 0x04) || !append(b, 0x18) || !putIssue(b, tx["Asset2"])) return false;  // (24/4)
    return true;
}

// AMMWithdraw (remove liquidity). Same shape as deposit but with LPTokenIn.
bool appendAmmWithdraw(Bytes& b, JsonObject tx, bool includeSig, const uint8_t* sig,
                       size_t sigLen) {
    uint8_t account[20], pubkey[33];
    if (!resolveSigner(tx, account, pubkey)) return false;
    const uint32_t lastLedger = readU32(tx["LastLedgerSequence"]);

    if (!append(b, 0x12) || !putU16(b, 37)) return false;  // AMMWithdraw
    if (!appendCommonUInts(b, tx)) return false;
    if (!append(b, 0x20) || !append(b, 0x1B) || !putU32(b, lastLedger)) return false;
    if (!tx["Amount"].isNull() &&
        (!append(b, 0x61) || !putAmountValue(b, tx["Amount"])))  // Amount (6/1)
        return false;
    if (!append(b, 0x68) || !putXrpAmount(b, tx["Fee"] | "")) return false;  // Fee (6/8)
    if (!tx["Amount2"].isNull() &&
        (!append(b, 0x6B) || !putAmountValue(b, tx["Amount2"])))  // Amount2 (6/11)
        return false;
    if (!tx["LPTokenIn"].isNull() &&
        (!append(b, 0x60) || !append(b, 0x1A) || !putAmountValue(b, tx["LPTokenIn"])))  // (6/26)
        return false;
    if (!tx["EPrice"].isNull() &&
        (!append(b, 0x60) || !append(b, 0x1B) || !putAmountValue(b, tx["EPrice"])))  // (6/27)
        return false;
    if (!appendPubAndAccount(b, pubkey, account, includeSig, sig, sigLen)) return false;
    if (!append(b, 0x03) || !append(b, 0x18) || !putIssue(b, tx["Asset"])) return false;   // (24/3)
    if (!append(b, 0x04) || !append(b, 0x18) || !putIssue(b, tx["Asset2"])) return false;  // (24/4)
    return true;
}

bool serialize(const char* type, Bytes& b, JsonObject tx, bool includeSig, const uint8_t* sig,
               size_t sigLen) {
    if (strcmp(type, "Payment") == 0) return appendPayment(b, tx, includeSig, sig, sigLen);
    if (strcmp(type, "TrustSet") == 0) return appendTrustSet(b, tx, includeSig, sig, sigLen);
    if (strcmp(type, "OfferCreate") == 0) return appendOfferCreate(b, tx, includeSig, sig, sigLen);
    if (strcmp(type, "OfferCancel") == 0) return appendOfferCancel(b, tx, includeSig, sig, sigLen);
    if (strcmp(type, "AccountSet") == 0) return appendAccountSet(b, tx, includeSig, sig, sigLen);
    if (strcmp(type, "AMMDeposit") == 0) return appendAmmDeposit(b, tx, includeSig, sig, sigLen);
    if (strcmp(type, "AMMWithdraw") == 0) return appendAmmWithdraw(b, tx, includeSig, sig, sigLen);
    return false;
}

JsonObject txObject(JsonDocument& doc) {
    if (doc["tx_json"].is<JsonObject>()) return doc["tx_json"].as<JsonObject>();
    return doc.as<JsonObject>();
}

void setReviewError(Review* review, Status status, const char* warning) {
    review->status = status;
    strncpy(review->warning, warning, sizeof(review->warning) - 1);
}

// Reject any field not in the per-type allowlist so the device never silently
// signs something it did not display (sign-what-you-see).
bool onlyAllowedFields(JsonObject tx, const char* const* allowed, size_t n, Review* review) {
    for (JsonPair kv : tx) {
        bool ok = false;
        for (size_t i = 0; i < n; ++i) {
            if (strcmp(kv.key().c_str(), allowed[i]) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            char w[80];
            snprintf(w, sizeof(w), "Field not allowed: %s", kv.key().c_str());
            setReviewError(review, Status::Invalid, w);
            return false;
        }
    }
    return true;
}

bool reviewJson(JsonObject tx, Review* review) {
    memset(review, 0, sizeof(*review));
    const char* type = tx["TransactionType"] | "";
    strncpy(review->type, type, sizeof(review->type) - 1);
    strncpy(review->network, "MAINNET", sizeof(review->network) - 1);

    if (!wallet::isUnlocked()) {
        setReviewError(review, Status::Invalid, "Unlock the wallet first");
        return false;
    }
    if (tx["SigningPubKey"].is<const char*>() || tx["TxnSignature"].is<const char*>() ||
        tx["Signers"].is<JsonArray>()) {
        setReviewError(review, Status::Invalid, "Pre-signed fields rejected");
        return false;
    }

    const char* account = tx["Account"] | "";
    strncpy(review->account, account, sizeof(review->account) - 1);
    if (strcmp(account, wallet::address()) != 0) {
        setReviewError(review, Status::Invalid, "Account does not match signer");
        return false;
    }
    if (!tx["Fee"].is<const char*>() || !tx["Sequence"].is<uint32_t>() ||
        !tx["LastLedgerSequence"].is<uint32_t>()) {
        setReviewError(review, Status::Invalid, "Missing Fee/Sequence/LastLedgerSequence");
        return false;
    }
    strncpy(review->fee, tx["Fee"] | "", sizeof(review->fee) - 1);
    review->sequence = readU32(tx["Sequence"]);
    review->lastLedger = readU32(tx["LastLedgerSequence"]);
    review->highFee = strtoul(review->fee, nullptr, 10) > HIGH_FEE_DROPS;
    if (tx["Flags"].is<uint32_t>()) {
        review->flags = readU32(tx["Flags"]);
        review->hasFlags = true;
    }
    if (!tx["SourceTag"].isNull() && !tx["SourceTag"].is<uint32_t>()) {
        setReviewError(review, Status::Invalid, "Invalid SourceTag");
        return false;
    }
    if (tx["SourceTag"].is<uint32_t>()) {
        review->sourceTag = readU32(tx["SourceTag"]);
        review->hasSourceTag = true;
    }

    if (strcmp(type, "Payment") == 0) {
        static const char* const allowed[] = {
            "TransactionType", "Account",    "Fee",        "Sequence", "LastLedgerSequence",
            "Flags",           "SourceTag",  "Amount",     "Destination", "DestinationTag",
            "SendMax",         "DeliverMin", "Paths"};
        if (!onlyAllowedFields(tx, allowed, sizeof(allowed) / sizeof(allowed[0]), review))
            return false;
        const bool amtXrp = tx["Amount"].is<const char*>();
        const bool amtIou = tx["Amount"].is<JsonObject>();
        if (!amtXrp && !amtIou) {
            setReviewError(review, Status::Invalid, "Invalid Amount");
            return false;
        }
        if (!tx["Destination"].is<const char*>()) {
            setReviewError(review, Status::Invalid, "Missing Destination");
            return false;
        }
        if (!tx["SendMax"].isNull() && !tx["SendMax"].is<const char*>() &&
            !tx["SendMax"].is<JsonObject>()) {
            setReviewError(review, Status::Invalid, "Invalid SendMax");
            return false;
        }
        if (!tx["DeliverMin"].isNull() && !tx["DeliverMin"].is<const char*>() &&
            !tx["DeliverMin"].is<JsonObject>()) {
            setReviewError(review, Status::Invalid, "Invalid DeliverMin");
            return false;
        }
        if (!tx["Paths"].isNull() && !tx["Paths"].is<JsonArray>()) {
            setReviewError(review, Status::Invalid, "Invalid Paths");
            return false;
        }
        char amt[40];
        formatAmount(tx["Amount"], amt, sizeof(amt));
        const bool selfSwap = strcmp(tx["Destination"] | "", account) == 0;
        const bool isSwap = selfSwap || amtIou || !tx["SendMax"].isNull();
        if (isSwap) {
            snprintf(review->primary, sizeof(review->primary), "Swap -> %s", amt);
            if (!tx["SendMax"].isNull()) {
                char send[40];
                formatAmount(tx["SendMax"], send, sizeof(send));
                snprintf(review->secondary, sizeof(review->secondary), "Max %s", send);
            } else {
                char shortDest[32];
                wallet::shortenAddress(tx["Destination"] | "", shortDest, sizeof(shortDest));
                snprintf(review->secondary, sizeof(review->secondary), "To %s", shortDest);
            }
        } else {
            snprintf(review->primary, sizeof(review->primary), "%s", amt);
            char shortDest[32];
            wallet::shortenAddress(tx["Destination"] | "", shortDest, sizeof(shortDest));
            snprintf(review->secondary, sizeof(review->secondary), "To %s", shortDest);
        }
        if (tx["DestinationTag"].is<uint32_t>()) {
            review->destinationTag = readU32(tx["DestinationTag"]);
            review->hasDestinationTag = true;
        }
    } else if (strcmp(type, "OfferCreate") == 0) {
        static const char* const allowed[] = {"TransactionType", "Account",   "Fee",
                                              "Sequence",        "LastLedgerSequence",
                                              "Flags",           "SourceTag", "TakerPays",
                                              "TakerGets",       "Expiration", "OfferSequence"};
        if (!onlyAllowedFields(tx, allowed, sizeof(allowed) / sizeof(allowed[0]), review))
            return false;
        const bool paysOk = tx["TakerPays"].is<const char*>() || tx["TakerPays"].is<JsonObject>();
        const bool getsOk = tx["TakerGets"].is<const char*>() || tx["TakerGets"].is<JsonObject>();
        if (!paysOk || !getsOk) {
            setReviewError(review, Status::Invalid, "Invalid TakerPays/TakerGets");
            return false;
        }
        char pays[40], gets[40];
        formatAmount(tx["TakerPays"], pays, sizeof(pays));
        formatAmount(tx["TakerGets"], gets, sizeof(gets));
        snprintf(review->primary, sizeof(review->primary), "Buy %s", pays);
        snprintf(review->secondary, sizeof(review->secondary), "Pay %s", gets);
    } else if (strcmp(type, "AMMDeposit") == 0 || strcmp(type, "AMMWithdraw") == 0) {
        const bool deposit = strcmp(type, "AMMDeposit") == 0;
        static const char* const allowedDep[] = {
            "TransactionType", "Account", "Fee",    "Sequence", "LastLedgerSequence", "Flags",
            "SourceTag",       "Asset",   "Asset2", "Amount",   "Amount2",            "EPrice",
            "LPTokenOut"};
        static const char* const allowedWit[] = {
            "TransactionType", "Account", "Fee",    "Sequence", "LastLedgerSequence", "Flags",
            "SourceTag",       "Asset",   "Asset2", "Amount",   "Amount2",            "EPrice",
            "LPTokenIn"};
        const char* const* allowed = deposit ? allowedDep : allowedWit;
        const size_t allowedN = deposit ? sizeof(allowedDep) / sizeof(allowedDep[0])
                                        : sizeof(allowedWit) / sizeof(allowedWit[0]);
        if (!onlyAllowedFields(tx, allowed, allowedN, review)) return false;
        if (!tx["Asset"].is<JsonObject>() || !tx["Asset2"].is<JsonObject>()) {
            setReviewError(review, Status::Invalid, "Missing Asset/Asset2");
            return false;
        }
        const char* lpField = deposit ? "LPTokenOut" : "LPTokenIn";
        if (tx["Amount"].isNull() && tx["Amount2"].isNull() && tx["EPrice"].isNull() &&
            tx[lpField].isNull()) {
            setReviewError(review, Status::Invalid, "No deposit/withdraw amount");
            return false;
        }
        char a1[20], a2[20];
        formatIssue(tx["Asset"], a1, sizeof(a1));
        formatIssue(tx["Asset2"], a2, sizeof(a2));
        snprintf(review->primary, sizeof(review->primary), "%s %s/%s",
                 deposit ? "Add LP" : "Remove LP", a1, a2);
        if (!tx["Amount"].isNull() && !tx["Amount2"].isNull()) {
            char x[40], y[40];
            formatAmount(tx["Amount"], x, sizeof(x));
            formatAmount(tx["Amount2"], y, sizeof(y));
            snprintf(review->secondary, sizeof(review->secondary), "%s + %s", x, y);
        } else if (!tx[lpField].isNull()) {
            char lp[40];
            formatAmount(tx[lpField], lp, sizeof(lp));
            snprintf(review->secondary, sizeof(review->secondary), "%s LP", lp);
        } else if (!tx["Amount"].isNull()) {
            formatAmount(tx["Amount"], review->secondary, sizeof(review->secondary));
        } else {
            formatAmount(tx["Amount2"], review->secondary, sizeof(review->secondary));
        }
    } else if (strcmp(type, "TrustSet") == 0) {
        static const char* const allowed[] = {"TransactionType", "Account", "Fee",
                                              "Sequence",        "LastLedgerSequence",
                                              "Flags",           "SourceTag", "LimitAmount"};
        if (!onlyAllowedFields(tx, allowed, sizeof(allowed) / sizeof(allowed[0]), review))
            return false;
        JsonObject limit = tx["LimitAmount"].as<JsonObject>();
        if (limit.isNull() || !limit["currency"].is<const char*>() ||
            !limit["issuer"].is<const char*>() || !limit["value"].is<const char*>()) {
            setReviewError(review, Status::Invalid, "Invalid LimitAmount");
            return false;
        }
        uint8_t probe[20];
        if (!fillCurrency(limit["currency"] | "", probe) ||
            !wallet::accountIdFromAddress(limit["issuer"] | "", probe)) {
            setReviewError(review, Status::Invalid, "Bad currency/issuer");
            return false;
        }
        snprintf(review->primary, sizeof(review->primary), "Limit %s %s", limit["value"] | "",
                 limit["currency"] | "");
        char shortIss[32];
        wallet::shortenAddress(limit["issuer"] | "", shortIss, sizeof(shortIss));
        snprintf(review->secondary, sizeof(review->secondary), "Issuer %s", shortIss);
    } else if (strcmp(type, "OfferCancel") == 0) {
        static const char* const allowed[] = {"TransactionType", "Account", "Fee",
                                              "Sequence",        "LastLedgerSequence",
                                              "Flags",           "SourceTag", "OfferSequence"};
        if (!onlyAllowedFields(tx, allowed, sizeof(allowed) / sizeof(allowed[0]), review))
            return false;
        if (!tx["OfferSequence"].is<uint32_t>()) {
            setReviewError(review, Status::Invalid, "Missing OfferSequence");
            return false;
        }
        strncpy(review->primary, "Cancel offer", sizeof(review->primary) - 1);
        snprintf(review->secondary, sizeof(review->secondary), "Offer #%u",
                 static_cast<unsigned>(readU32(tx["OfferSequence"])));
    } else if (strcmp(type, "AccountSet") == 0) {
        static const char* const allowed[] = {"TransactionType", "Account", "Fee",
                                              "Sequence",        "LastLedgerSequence", "Flags",
                                              "SourceTag",       "SetFlag", "ClearFlag"};
        if (!onlyAllowedFields(tx, allowed, sizeof(allowed) / sizeof(allowed[0]), review))
            return false;
        strncpy(review->primary, "Account settings", sizeof(review->primary) - 1);
        const bool hasSet = tx["SetFlag"].is<uint32_t>();
        const bool hasClear = tx["ClearFlag"].is<uint32_t>();
        if (hasSet && hasClear) {
            snprintf(review->secondary, sizeof(review->secondary), "Set %u / Clear %u",
                     static_cast<unsigned>(readU32(tx["SetFlag"])),
                     static_cast<unsigned>(readU32(tx["ClearFlag"])));
        } else if (hasSet) {
            snprintf(review->secondary, sizeof(review->secondary), "SetFlag %u",
                     static_cast<unsigned>(readU32(tx["SetFlag"])));
        } else if (hasClear) {
            snprintf(review->secondary, sizeof(review->secondary), "ClearFlag %u",
                     static_cast<unsigned>(readU32(tx["ClearFlag"])));
        } else {
            strncpy(review->secondary, "Flags only", sizeof(review->secondary) - 1);
        }
    } else {
        setReviewError(review, Status::Unsupported, "Transaction type not supported");
        return false;
    }

    review->status = Status::Ready;
    strncpy(review->warning, review->highFee ? "High fee - verify before signing" : "Ready to sign",
            sizeof(review->warning) - 1);
    return true;
}

}  // namespace

const char* statusText(Status status) {
    switch (status) {
        case Status::Ready:
            return "READY";
        case Status::Invalid:
            return "INVALID";
        case Status::Unsupported:
            return "UNSUPPORTED";
        case Status::Signed:
            return "SIGNED";
        default:
            return "UNKNOWN";
    }
}

bool parseAndReview(const char* payload, Review* review) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        memset(review, 0, sizeof(*review));
        char w[80];
        snprintf(w, sizeof(w), "JSON parse failed: %s", err.c_str());
        setReviewError(review, Status::Invalid, w);
        return false;
    }
    const char* network = doc["network"] | "mainnet";
    if (strcmp(network, "mainnet") != 0) {
        memset(review, 0, sizeof(*review));
        setReviewError(review, Status::Invalid, "Only mainnet payloads accepted");
        return false;
    }
    JsonObject tx = txObject(doc);
    return reviewJson(tx, review);
}

bool signReviewedPayload(const char* payload, SignedTx* out) {
    memset(out, 0, sizeof(*out));
    static Review review;
    static JsonDocument doc;
    doc.clear();
    if (deserializeJson(doc, payload)) {
        strncpy(out->error, "JSON parse failed", sizeof(out->error) - 1);
        return false;
    }
    JsonObject tx = txObject(doc);
    memset(&review, 0, sizeof(review));
    if (!reviewJson(tx, &review)) {
        strncpy(out->error, review.warning, sizeof(out->error) - 1);
        return false;
    }

    // Work buffers in BSS — keep ~6 KB off the loop-task stack during ECDSA.
    static Bytes signing;
    static Bytes signedBlob;
    static Bytes hashInput;
    signing.len = 0;
    signedBlob.len = 0;
    hashInput.len = 0;

    append(signing, STX_PREFIX, sizeof(STX_PREFIX));
    if (!serialize(review.type, signing, tx, false, nullptr, 0)) {
        strncpy(out->error, "Signing serialization failed", sizeof(out->error) - 1);
        return false;
    }

    uint8_t sig[96];
    size_t sigLen = sizeof(sig);
    if (!wallet::signSha512Half(signing.data, signing.len, sig, &sigLen)) {
        strncpy(out->error, "ECDSA signing failed", sizeof(out->error) - 1);
        return false;
    }

    if (!serialize(review.type, signedBlob, tx, true, sig, sigLen)) {
        strncpy(out->error, "Signed serialization failed", sizeof(out->error) - 1);
        return false;
    }
    crypto::bytesToHex(signedBlob.data, signedBlob.len, out->txBlob, sizeof(out->txBlob));

    append(hashInput, TXN_PREFIX, sizeof(TXN_PREFIX));
    append(hashInput, signedBlob.data, signedBlob.len);
    crypto::sha512HalfHex(hashInput.data, hashInput.len, out->txHash, sizeof(out->txHash));

    JsonDocument response;
    response["protocol"] = "XRPL-AQ/1";
    response["kind"] = "signed";
    response["network"] = "mainnet";
    response["tx_blob"] = out->txBlob;
    response["tx_hash"] = out->txHash;
    serializeJson(response, out->json, sizeof(out->json));
    return true;
}

}  // namespace txsigner
