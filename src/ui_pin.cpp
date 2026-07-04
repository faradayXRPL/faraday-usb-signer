#include "ui_internal.h"

#include <Arduino.h>
#include <lvgl.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "theme.h"
#include "wallet.h"

namespace ui {
namespace internal {

namespace {

struct DeferredOp {
    PinFlow flow;
    char pinFirst[16];
    char importStage[40];
};

DeferredOp* g_deferredOp = nullptr;

void setPinMsg(const char* text) {
    if (g_pinMsg) {
        lv_label_set_text(g_pinMsg, text);
        lv_obj_set_style_text_color(g_pinMsg, theme::Danger, 0);
    }
}

void wipePinState() {
    memset(g_pinBuf, 0, sizeof(g_pinBuf));
    memset(g_pinFirst, 0, sizeof(g_pinFirst));
}

void updatePinDots() {
    if (!g_pinDots) return;
    const int len = static_cast<int>(strlen(g_pinBuf));
    char dots[2 * 16 + 1];
    int p = 0;
    for (int i = 0; i < len && i < 16; ++i) {
        dots[p++] = '*';
        if (i + 1 < len) dots[p++] = ' ';
    }
    dots[p] = '\0';
    lv_label_set_text(g_pinDots, len > 0 ? dots : "- - - -");
}

void pinSubmit();

void onPinKey(lv_event_t* e) {
    lv_obj_t* mx = lv_event_get_target(e);
    const uint32_t id = lv_btnmatrix_get_selected_btn(mx);
    const char* txt = lv_btnmatrix_get_btn_text(mx, id);
    if (!txt) return;
    const int len = static_cast<int>(strlen(g_pinBuf));
    if (strcmp(txt, "DEL") == 0) {
        if (len > 0) g_pinBuf[len - 1] = '\0';
        updatePinDots();
    } else if (strcmp(txt, "OK") == 0) {
        pinSubmit();
    } else if (txt[0] >= '0' && txt[0] <= '9') {
        if (len < cfg::PIN_MAX_LEN) {
            g_pinBuf[len] = txt[0];
            g_pinBuf[len + 1] = '\0';
            updatePinDots();
        }
    }
}

void deferredPinOp(lv_timer_t* t) {
    DeferredOp* op = g_deferredOp;
    g_deferredOp = nullptr;
    lv_timer_del(t);
    if (!op) return;
    switch (op->flow) {
        case PinFlow::Unlock:
            if (wallet::unlock(g_pinBuf)) {
                wipePinState();
                showHome();
            } else if (!wallet::hasWallet()) {
                wipePinState();
                showOnboarding();
            } else {
                const int left = cfg::MAX_PIN_FAILS - wallet::failedAttempts();
                char m[44];
                snprintf(m, sizeof(m), "Wrong PIN - %d attempts left", left);
                setPinMsg(m);
                g_pinBuf[0] = '\0';
                updatePinDots();
            }
            break;
        case PinFlow::CreateConfirm:
            if (wallet::createWallet(op->pinFirst)) {
                wipePinState();
                showSeedBackup();
            } else {
                setPinMsg("Wallet creation failed");
            }
            break;
        case PinFlow::ImportConfirm:
            if (wallet::importSeed(op->importStage, op->pinFirst)) {
                memset(op->importStage, 0, sizeof(op->importStage));
                wipePinState();
                showHome();
            } else {
                setPinMsg("Invalid family seed");
            }
            break;
        case PinFlow::ChangeAuthorize: {
            char authPin[16];
            strncpy(authPin, op->pinFirst, sizeof(authPin) - 1);
            authPin[sizeof(authPin) - 1] = '\0';
            const bool ok = wallet::unlock(authPin);
            if (ok) {
                g_pinBuf[0] = '\0';
                updatePinDots();
                showPin("NEW PIN", "Choose a new device PIN", PinFlow::ChangeSet);
            } else if (!wallet::hasWallet()) {
                wipePinState();
                showOnboarding();
            } else {
                const int left = cfg::MAX_PIN_FAILS - wallet::failedAttempts();
                char m[44];
                snprintf(m, sizeof(m), "Wrong current PIN - %d attempts left", left);
                setPinMsg(m);
                g_pinBuf[0] = '\0';
                updatePinDots();
            }
            break;
        }
        case PinFlow::ClearAuthorize: {
            char authPin[16];
            strncpy(authPin, op->pinFirst, sizeof(authPin) - 1);
            authPin[sizeof(authPin) - 1] = '\0';
            const bool ok = wallet::unlock(authPin);
            if (ok) {
                wallet::clearWallet();
                wipePinState();
                markIntroSeen(false);
                showIntro(0);
            } else if (!wallet::hasWallet()) {
                wipePinState();
                showOnboarding();
            } else {
                const int left = cfg::MAX_PIN_FAILS - wallet::failedAttempts();
                char m[44];
                snprintf(m, sizeof(m), "Wrong PIN - %d attempts left", left);
                setPinMsg(m);
                g_pinBuf[0] = '\0';
                updatePinDots();
            }
            break;
        }
        case PinFlow::ChangeConfirm:
            wallet::changePin(op->pinFirst);
            wipePinState();
            onSettings(nullptr);
            break;
        default:
            break;
    }
    delete op;
}

void pinSubmit() {
    if (static_cast<int>(strlen(g_pinBuf)) < cfg::PIN_MIN_LEN) {
        char m[40];
        snprintf(m, sizeof(m), "PIN needs %d+ digits", cfg::PIN_MIN_LEN);
        setPinMsg(m);
        return;
    }
    const char* statusText = nullptr;
    const PinFlow targetFlow = g_pinFlow;
    switch (g_pinFlow) {
        case PinFlow::Unlock:
            statusText = "Decrypting wallet...";
            break;
        case PinFlow::CreateSet:
            strcpy(g_pinFirst, g_pinBuf);
            showPin("CONFIRM PIN", "Re-enter your PIN", PinFlow::CreateConfirm);
            return;
        case PinFlow::CreateConfirm:
            if (strcmp(g_pinFirst, g_pinBuf) != 0) {
                showPin("SET PIN", "PINs did not match - try again", PinFlow::CreateSet);
                return;
            }
            statusText = "Creating wallet...";
            break;
        case PinFlow::ImportSet:
            strcpy(g_pinFirst, g_pinBuf);
            showPin("CONFIRM PIN", "Re-enter your PIN", PinFlow::ImportConfirm);
            return;
        case PinFlow::ImportConfirm:
            if (strcmp(g_pinFirst, g_pinBuf) != 0) {
                showPin("SET PIN", "PINs did not match - try again", PinFlow::ImportSet);
                return;
            }
            statusText = "Importing seed...";
            break;
        case PinFlow::ChangeAuthorize:
            statusText = "Verifying current PIN...";
            break;
        case PinFlow::ClearAuthorize:
            statusText = "Verifying PIN...";
            break;
        case PinFlow::ChangeSet:
            strcpy(g_pinFirst, g_pinBuf);
            showPin("CONFIRM PIN", "Re-enter the new PIN", PinFlow::ChangeConfirm);
            return;
        case PinFlow::ChangeConfirm:
            if (strcmp(g_pinFirst, g_pinBuf) != 0) {
                showPin("NEW PIN", "PINs did not match - try again", PinFlow::ChangeSet);
                return;
            }
            statusText = "Re-encrypting wallet...";
            break;
    }
    setPinMsg(statusText);
    auto* op = new DeferredOp();
    op->flow = targetFlow;
    if (targetFlow == PinFlow::ChangeAuthorize || targetFlow == PinFlow::ClearAuthorize) {
        strncpy(op->pinFirst, g_pinBuf, sizeof(op->pinFirst) - 1);
    } else {
        strncpy(op->pinFirst, g_pinFirst, sizeof(op->pinFirst) - 1);
    }
    op->pinFirst[sizeof(op->pinFirst) - 1] = '\0';
    strncpy(op->importStage, g_importStage, sizeof(op->importStage) - 1);
    op->importStage[sizeof(op->importStage) - 1] = '\0';
    g_deferredOp = op;
    lv_timer_t* timer = lv_timer_create(deferredPinOp, 100, nullptr);
    lv_timer_set_repeat_count(timer, 1);
}

}  // namespace

void showPin(const char* titleText, const char* subtitle, PinFlow flow) {
    g_pinFlow = flow;
    g_pinBuf[0] = '\0';

    lv_obj_t* root = screenRoot();
    heading(root, titleText);
    lv_obj_t* sub = fine(root, subtitle);
    lv_obj_set_style_text_color(sub, theme::Muted, 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_LEFT, 0);

    g_pinMsg = makeText(root, "");
    theme::applyBodySm(g_pinMsg);
    lv_obj_set_style_text_color(g_pinMsg, theme::Danger, 0);
    lv_obj_set_style_text_align(g_pinMsg, LV_TEXT_ALIGN_LEFT, 0);

    g_pinDots = makeText(root, "");
    theme::applyTitle(g_pinDots);
    lv_obj_set_style_text_color(g_pinDots, theme::Accent, 0);

    static const char* kMap[] = {"1", "2", "3", "\n",       "4", "5", "6", "\n",
                                 "7", "8", "9", "\n", "DEL", "0", "OK", ""};
    lv_obj_t* mx = lv_btnmatrix_create(root);
    lv_btnmatrix_set_map(mx, kMap);
    lv_obj_set_width(mx, lv_pct(100));
    lv_obj_set_flex_grow(mx, 1);
    lv_obj_set_style_text_font(mx, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_opa(mx, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mx, 0, 0);
    lv_obj_set_style_pad_all(mx, 0, 0);
    lv_obj_set_style_pad_row(mx, 8, 0);
    lv_obj_set_style_pad_column(mx, 8, 0);
    lv_obj_set_style_bg_color(mx, theme::SurfaceHi, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(mx, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(mx, 10, LV_PART_ITEMS);
    lv_obj_set_style_border_color(mx, theme::Line, LV_PART_ITEMS);
    lv_obj_set_style_border_width(mx, 1, LV_PART_ITEMS);
    lv_obj_set_style_text_color(mx, theme::Text, LV_PART_ITEMS);
    lv_obj_set_style_text_font(mx, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(mx, theme::Accent, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(mx, theme::Void, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_event_cb(mx, onPinKey, LV_EVENT_VALUE_CHANGED, nullptr);

    updatePinDots();
}

void showPinUnlock() {
    char sub[48] = "Enter your device PIN";
    const int fails = wallet::failedAttempts();
    if (fails > 0) {
        snprintf(sub, sizeof(sub), "%d of %d attempts used", fails, cfg::MAX_PIN_FAILS);
    }
    showPin("ENTER PIN", sub, PinFlow::Unlock);
}

}  // namespace internal
}  // namespace ui
