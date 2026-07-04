#include "ui_internal.h"

#include <Arduino.h>
#include <lvgl.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "fonts.h"
#include "theme.h"
#include "wallet.h"

namespace ui {
namespace internal {

namespace {

void onCreateImportKeyboard();
void showSystemSettings();

void onCreate(lv_event_t*) {
    showPin("SET PIN", "Choose a 4-8 digit device PIN", PinFlow::CreateSet);
}

void onImport(lv_event_t*) {
    onCreateImportKeyboard();
}

void onCreateImportKeyboard() {
    lv_obj_t* root = screenRoot();

    topBar(root, "IMPORT SEED", [](lv_event_t*) { showOnboarding(); });
    fine(root, "Enter your 29-character XRPL family seed");

    lv_obj_t* ta = lv_textarea_create(root);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_height(ta, 48);
    lv_obj_set_style_text_font(ta, &faraday_inter_14, 0);
    lv_obj_set_style_text_color(ta, theme::Text, 0);
    lv_obj_set_style_bg_color(ta, theme::SurfaceHi, 0);
    lv_obj_set_style_border_color(ta, theme::Line, 0);
    lv_obj_set_style_border_color(ta, theme::Accent, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(ta, 10, 0);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 29);
    lv_textarea_set_accepted_chars(ta, "s0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    lv_textarea_set_text(ta, "");

    lv_obj_t* kb = lv_keyboard_create(root);
    lv_obj_set_width(kb, lv_pct(100));
    lv_obj_set_flex_grow(kb, 1);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_14, 0);
    lv_keyboard_set_textarea(kb, ta);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_UPPER);

    lv_obj_t* row = lv_obj_create(root);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    theme::styleTransparent(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* applyBtn = button(row, "IMPORT", nullptr, Btn::Accent);
    lv_obj_set_flex_grow(applyBtn, 1);
    lv_obj_add_event_cb(
        applyBtn,
        [](lv_event_t* e) {
            lv_obj_t* taRef = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
            const char* text = taRef ? lv_textarea_get_text(taRef) : "";
            if (text && strlen(text) >= 29) onSeedEnter(text);
        },
        LV_EVENT_CLICKED, ta);

    lv_obj_t* cancelBtn = button(row, "CANCEL", [](lv_event_t*) { showOnboarding(); });
    lv_obj_set_flex_grow(cancelBtn, 1);
}

void closeQr(lv_event_t*) {
    g_qr = nullptr;
    showHome();
}

void qrShowAddress() {
    showQrScreen("RECEIVE", wallet::address(), closeQr,
                 "Scan to send XRP or issued assets to this wallet.", wallet::address());
}

void onShowAddress(lv_event_t*) {
    qrShowAddress();
}

void onRestart(lv_event_t*) {
    ESP.restart();
}

void showChangePinConfirm() {
    lv_obj_t* p = panel();
    heading(p, "CHANGE PIN?");
    lv_obj_t* b =
        fine(p, "Do you really want to change your PIN? You will need to enter your current PIN first.");
    lv_obj_set_style_text_color(b, theme::Muted, 0);
    button(p, "YES, CHANGE",
           [](lv_event_t*) {
               showPin("ENTER CURRENT PIN", "Enter your current PIN to authorize", PinFlow::ChangeAuthorize);
           },
           Btn::Accent);
    button(p, "CANCEL", [](lv_event_t*) { onSettings(nullptr); });
}

void showInfo() {
    lv_obj_t* p = panel();
    heading(p, "INFO");
    lv_obj_t* t = fine(p, "FARADAY XRPL Signer");
    lv_obj_set_style_text_color(t, theme::Text, 0);
    lv_obj_t* a = makeText(p, "Programmed by Daniele Abbattista");
    theme::applyBodySm(a);
    lv_obj_set_style_text_color(a, theme::Text, 0);
    lv_obj_t* l1 = makeText(p, "https://github.com/faradayXRPL");
    lv_obj_set_style_text_color(l1, theme::Accent, 0);
    lv_obj_set_style_text_decor(l1, LV_TEXT_DECOR_UNDERLINE, 0);
    lv_obj_t* l2 = makeText(p, "faradayxrpl@pm.me");
    lv_obj_set_style_text_color(l2, theme::Accent, 0);
    lv_obj_set_style_text_decor(l2, LV_TEXT_DECOR_UNDERLINE, 0);
    button(p, "BACK", [](lv_event_t*) { onSettings(nullptr); });
}

void showClearWalletConfirm() {
    lv_obj_t* p = panel();
    heading(p, "CLEAR WALLET?");
    lv_obj_t* b = fine(p,
                       "Do you really want to clear this wallet? All key material will be destroyed. "
                       "This cannot be undone.");
    lv_obj_set_style_text_color(b, theme::Muted, 0);
    button(p, "YES, CLEAR",
           [](lv_event_t*) {
               showPin("CONFIRM CLEAR", "Enter your PIN to authorize", PinFlow::ClearAuthorize);
           },
           Btn::Accent);
    button(p, "CANCEL", [](lv_event_t*) { showSystemSettings(); });
}

void showSystemSettings() {
    lv_obj_t* p = panel();
    heading(p, "SYSTEM");
    char ver[48];
    snprintf(ver, sizeof(ver), "%s %s", cfg::FW_NAME, cfg::FW_VERSION);
    lv_obj_t* v = fine(p, ver);
    lv_obj_set_style_text_color(v, theme::Muted, 0);
    button(p, "CLEAR WALLET", [](lv_event_t*) { showClearWalletConfirm(); }, Btn::Danger);
    button(p, "RESTART DEVICE", onRestart);
    {
        lv_obj_t* infoBtn = button(p, "INFO", [](lv_event_t*) { showInfo(); });
        lv_obj_t* infoLbl = lv_obj_get_child(infoBtn, 0);
        lv_obj_set_style_text_color(infoLbl, lv_color_hex(0xFFD60A), 0);
    }
    button(p, "BACK", [](lv_event_t*) { onSettings(nullptr); });
}

void onSeedBackupDone(lv_event_t*) {
    if (g_seedLabel) {
        lv_label_set_text(g_seedLabel, "");
        g_seedLabel = nullptr;
    }
    showHome();
}

}  // namespace

void onSettings(lv_event_t*) {
    lv_obj_t* p = panel();
    heading(p, "SETTINGS");
    char ver[48];
    snprintf(ver, sizeof(ver), "%s %s", cfg::FW_NAME, cfg::FW_VERSION);
    lv_obj_t* v = fine(p, ver);
    lv_obj_set_style_text_color(v, theme::Muted, 0);
    button(p, "LOCK NOW",
           [](lv_event_t*) {
               wallet::lock();
               showPinUnlock();
           },
           Btn::Accent);
    button(p, "CHANGE PIN", [](lv_event_t*) { showChangePinConfirm(); });
    button(p, "REPLAY INTRO",
           [](lv_event_t*) {
               markIntroSeen(false);
               showIntro(0);
           });
    button(p, "SYSTEM SETTINGS", [](lv_event_t*) { showSystemSettings(); });
    button(p, "BACK", [](lv_event_t*) { showHome(); });
}

void showHome() {
    lv_obj_t* root = screenRoot();

    lv_obj_t* headRow = lv_obj_create(root);
    lv_obj_set_width(headRow, lv_pct(100));
    lv_obj_set_height(headRow, LV_SIZE_CONTENT);
    theme::styleTransparent(headRow);
    lv_obj_set_flex_flow(headRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(headRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(headRow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* brand = title(headRow, "FARADAY");
    lv_obj_set_width(brand, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(brand, LV_TEXT_ALIGN_LEFT, 0);

    lv_obj_t* gear = button(headRow, "SET", onSettings, Btn::Secondary);
    lv_obj_set_width(gear, 48);
    lv_obj_set_height(gear, 40);

    lv_obj_t* addrCard = lv_obj_create(root);
    lv_obj_set_width(addrCard, lv_pct(100));
    lv_obj_set_height(addrCard, LV_SIZE_CONTENT);
    theme::stylePanel(addrCard);
    lv_obj_set_style_bg_color(addrCard, theme::SurfaceHi, 0);
    lv_obj_set_style_pad_all(addrCard, 14, 0);
    lv_obj_clear_flag(addrCard, LV_OBJ_FLAG_SCROLLABLE);

    char shortAddr[32];
    wallet::shortenAddress(wallet::address(), shortAddr, sizeof(shortAddr));
    lv_obj_t* addr = lv_label_create(addrCard);
    lv_label_set_text(addr, shortAddr);
    lv_obj_set_width(addr, lv_pct(100));
    theme::applyAmount(addr);
    lv_obj_set_style_text_align(addr, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(addr, theme::Silver, 0);

    lv_obj_t* actions = lv_obj_create(root);
    lv_obj_set_width(actions, lv_pct(100));
    lv_obj_set_flex_grow(actions, 1);
    theme::styleTransparent(actions);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(actions, 10, 0);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* recv = button(actions, "RECEIVE", onShowAddress);
    lv_obj_set_height(recv, 56);

    lv_obj_t* footer = lv_obj_create(root);
    lv_obj_set_width(footer, lv_pct(100));
    lv_obj_set_height(footer, LV_SIZE_CONTENT);
    theme::styleTransparent(footer);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(footer, 8, 0);
    lv_obj_set_style_pad_top(footer, 6, 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    g_homeConnectDot = lv_obj_create(footer);
    lv_obj_set_size(g_homeConnectDot, 8, 8);
    lv_obj_set_style_radius(g_homeConnectDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_homeConnectDot, theme::Danger, 0);
    lv_obj_set_style_border_width(g_homeConnectDot, 0, 0);
    lv_obj_clear_flag(g_homeConnectDot, LV_OBJ_FLAG_SCROLLABLE);

    g_homeConnectLabel = fine(footer, "Not connected");
    lv_obj_set_style_text_color(g_homeConnectLabel, theme::Muted, 0);
}

void showOnboarding() {
    lv_obj_t* p = panel();
    title(p, "FARADAY");
    lv_obj_t* tag = makeText(p, "YOUR KEYS. OFFLINE. SECURE.");
    theme::applyLabel(tag);
    lv_obj_set_style_text_color(tag, theme::Accent, 0);
    body(p, "Create or import an XRPL seed. This device never connects to a network.");

    lv_obj_t* spacer = lv_obj_create(p);
    lv_obj_set_width(spacer, lv_pct(100));
    lv_obj_set_height(spacer, 1);
    theme::styleTransparent(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);

    button(p, "CREATE WALLET", onCreate, Btn::Accent);
    button(p, "IMPORT SEED", onImport);
}

void showSeedBackup() {
    char seed[40];
    if (!wallet::exportFamilySeed(seed, sizeof(seed))) {
        showHome();
        return;
    }

    lv_obj_t* p = panel();
    lv_obj_set_style_pad_all(p, 12, 0);
    heading(p, "BACKUP SEED");

    lv_obj_t* warn = body(p,
                          "Write this down and store it offline. It is shown ONCE and is the only "
                          "way to restore this wallet.");
    lv_obj_set_style_text_color(warn, theme::Danger, 0);

    lv_obj_t* box = lv_obj_create(p);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    theme::stylePanel(box);
    lv_obj_set_style_bg_color(box, theme::SurfaceHi, 0);
    lv_obj_set_style_border_color(box, theme::Accent, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    g_seedLabel = lv_label_create(box);
    lv_label_set_text(g_seedLabel, seed);
    lv_obj_set_width(g_seedLabel, lv_pct(100));
    lv_label_set_long_mode(g_seedLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(g_seedLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_seedLabel, &faraday_inter_16, 0);
    lv_obj_set_style_text_color(g_seedLabel, theme::Accent, 0);
    lv_obj_set_style_text_letter_space(g_seedLabel, 1, 0);

    button(p, "I'VE WRITTEN IT DOWN", onSeedBackupDone, Btn::Accent);

    memset(seed, 0, sizeof(seed));
}

void onSeedEnter(const char* seed) {
    strncpy(g_importStage, seed, sizeof(g_importStage) - 1);
    g_importStage[sizeof(g_importStage) - 1] = '\0';
    showPin("SET PIN", "Choose a PIN for this wallet", PinFlow::ImportSet);
}

}  // namespace internal
}  // namespace ui
