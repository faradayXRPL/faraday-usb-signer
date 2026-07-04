#pragma once

namespace ui {

void build();
void playSplash();
void tick();

// True after splash/intro — safe to accept UNSIGNED over USB.
bool readyForUsb();

// Called when USB delivers an unsigned transaction JSON payload.
void submitUnsignedTx(const char* unsignedJson);

}  // namespace ui
