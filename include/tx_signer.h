#pragma once

#include <Arduino.h>

namespace txsigner {

inline constexpr uint8_t MAX_REVIEW_FIELDS = 28;
inline constexpr size_t REVIEW_FIELD_NAME_LEN = 28;
inline constexpr size_t REVIEW_FIELD_VALUE_LEN = 128;

enum class Status {
    Ready,
    Invalid,
    Unsupported,
    Signed,
};

struct ReviewField {
    char name[REVIEW_FIELD_NAME_LEN] = "";
    char value[REVIEW_FIELD_VALUE_LEN] = "";
};

struct Review {
    Status status = Status::Ready;
    char type[24] = "";
    char account[40] = "";
    char primary[48] = "";    // main line, e.g. "100000000 drops" / "1000 USD"
    char secondary[48] = "";  // context line, e.g. "To rXXX..." / "Issuer rYYY..."
    char fee[24] = "";
    char network[12] = "";
    uint32_t sequence = 0;
    uint32_t lastLedger = 0;
    uint32_t flags = 0;
    bool hasFlags = false;
    uint32_t sourceTag = 0;
    bool hasSourceTag = false;
    uint32_t destinationTag = 0;
    bool hasDestinationTag = false;
    bool highFee = false;
    uint8_t fieldCount = 0;
    ReviewField fields[MAX_REVIEW_FIELDS];
    char warning[80] = "";
};

struct SignedTx {
    char json[4096] = "";
    char txBlob[3072] = "";
    char txHash[72] = "";
    char error[96] = "";
};

bool parseAndReview(const char* payload, Review* review);
bool signReviewedPayload(const char* payload, SignedTx* out);
const char* statusText(Status status);

}  // namespace txsigner
