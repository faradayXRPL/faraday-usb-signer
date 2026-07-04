#pragma once

namespace usblink {

void begin();
void tick();

// True while a host has communicated over USB within the last few seconds.
bool hostActive();

void emitSigned(const char* json);
void emitRejected();
void emitError(const char* message);

}  // namespace usblink
