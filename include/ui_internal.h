#pragma once

#include <lvgl.h>

#include "config.h"

namespace ui {
namespace internal {

enum class PinFlow {
    Unlock,
    CreateSet,
    CreateConfirm,
    ImportSet,
    ImportConfirm,
    ChangeAuthorize,
    ChangeSet,
    ChangeConfirm,
    ClearAuthorize,
};

enum class Btn { Primary, Secondary, Accent, Danger };

extern lv_obj_t* g_screen;
extern lv_obj_t* g_qr;
extern lv_obj_t* g_seedLabel;
extern lv_obj_t* g_homeConnectDot;
extern lv_obj_t* g_homeConnectLabel;
extern bool g_splashDone;
extern char g_pendingPayload[cfg::MAX_TX_JSON + 1];
extern char g_signedJson[cfg::MAX_TX_JSON + 1];
extern bool g_signPending;
extern char g_importStage[40];
extern int g_introStep;

extern PinFlow g_pinFlow;
extern char g_pinBuf[16];
extern char g_pinFirst[16];
extern lv_obj_t* g_pinDots;
extern lv_obj_t* g_pinMsg;

void clear();
lv_obj_t* panel();

lv_obj_t* makeText(lv_obj_t* parent, const char* text);
lv_obj_t* title(lv_obj_t* parent, const char* text);
lv_obj_t* heading(lv_obj_t* parent, const char* text);
lv_obj_t* body(lv_obj_t* parent, const char* text);
lv_obj_t* fine(lv_obj_t* parent, const char* text);
lv_obj_t* button(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                 Btn variant = Btn::Secondary, lv_event_code_t code = LV_EVENT_CLICKED);

lv_obj_t* screenRoot();
lv_obj_t* topBar(lv_obj_t* parent, const char* title, lv_event_cb_t backCb);
void showQrScreen(const char* title, const char* payload, lv_event_cb_t closeCb,
                  const char* hint = nullptr, const char* footer = nullptr,
                  bool successTitle = false);

bool introSeen();
void markIntroSeen(bool seen);

void route();
void showHome();
void showOnboarding();
void showIntro(int step);
void finishIntro();
void showSeedBackup();
void showPin(const char* title, const char* subtitle, PinFlow flow);
void showPinUnlock();
void onSettings(lv_event_t*);
void onSeedEnter(const char* seed);
void submitUnsignedTxImpl(const char* payload);
void processPendingSign();

}  // namespace internal
}  // namespace ui
