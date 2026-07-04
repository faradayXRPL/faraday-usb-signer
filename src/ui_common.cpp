#include "ui_internal.h"

#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "fonts.h"
#include "logo_faraday.h"
#include "theme.h"
#include "usb_link.h"
#include "wallet.h"

namespace ui {
namespace internal {

lv_obj_t* g_screen = nullptr;
lv_obj_t* g_qr = nullptr;
lv_obj_t* g_seedLabel = nullptr;
lv_obj_t* g_homeConnectDot = nullptr;
lv_obj_t* g_homeConnectLabel = nullptr;
bool g_splashDone = false;
char g_pendingPayload[cfg::MAX_TX_JSON + 1] = "";
char g_signedJson[cfg::MAX_TX_JSON + 1] = "";
bool g_signPending = false;
char g_importStage[40] = "";
int g_introStep = 0;

PinFlow g_pinFlow = PinFlow::Unlock;
char g_pinBuf[16] = "";
char g_pinFirst[16] = "";
lv_obj_t* g_pinDots = nullptr;
lv_obj_t* g_pinMsg = nullptr;

bool introSeen() {
    Preferences prefs;
    prefs.begin("faraday", true);
    const bool seen = prefs.getBool("intro", false);
    prefs.end();
    return seen;
}

void markIntroSeen(bool seen) {
    Preferences prefs;
    prefs.begin("faraday", false);
    prefs.putBool("intro", seen);
    prefs.end();
}

void clear() {
    lv_obj_clean(lv_scr_act());
    g_screen = lv_scr_act();
    g_qr = nullptr;
    g_seedLabel = nullptr;
    g_homeConnectDot = nullptr;
    g_homeConnectLabel = nullptr;
    g_pinDots = nullptr;
    g_pinMsg = nullptr;
    theme::styleVoid(g_screen);
    theme::makeAccentBar(g_screen);
}

lv_obj_t* panel() {
    clear();
    lv_obj_t* p = lv_obj_create(g_screen);
    lv_obj_set_size(p, cfg::PANEL_WIDTH, cfg::PANEL_HEIGHT);
    lv_obj_align(p, LV_ALIGN_CENTER, 0, 1);
    theme::stylePanel(p);
    lv_obj_set_style_pad_all(p, cfg::UI_PAD, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(p, 10, 0);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    return p;
}

lv_obj_t* screenRoot() {
    clear();
    lv_obj_t* root = lv_obj_create(g_screen);
    lv_obj_set_size(root, cfg::SCREEN_WIDTH, cfg::SCREEN_HEIGHT - 2);
    lv_obj_align(root, LV_ALIGN_BOTTOM_MID, 0, 0);
    theme::styleVoid(root);
    lv_obj_set_style_pad_all(root, cfg::UI_EDGE, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(root, cfg::UI_PAD, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    return root;
}

lv_obj_t* topBar(lv_obj_t* parent, const char* title, lv_event_cb_t backCb) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, cfg::UI_TOP_BAR_H);
    theme::styleTransparent(bar);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = button(bar, "< BACK", backCb);
    lv_obj_set_width(back, 108);
    lv_obj_set_height(back, 36);

    lv_obj_t* t = lv_label_create(bar);
    lv_label_set_text(t, title);
    theme::applyHeading(t);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_flex_grow(t, 1);

    return bar;
}

void showQrScreen(const char* title, const char* payload, lv_event_cb_t closeCb,
                  const char* hint, const char* footer, bool successTitle) {
    clear();

    lv_obj_t* root = lv_obj_create(g_screen);
    lv_obj_set_size(root, cfg::SCREEN_WIDTH, cfg::SCREEN_HEIGHT - 2);
    lv_obj_align(root, LV_ALIGN_BOTTOM_MID, 0, 0);
    theme::styleVoid(root);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(root, cfg::UI_EDGE, 0);
    lv_obj_set_style_pad_column(root, cfg::UI_PAD, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* qrCard = lv_obj_create(root);
    lv_obj_set_size(qrCard, cfg::UI_QR_SIDE, lv_pct(100));
    theme::stylePanel(qrCard);
    lv_obj_set_style_bg_color(qrCard, theme::QrBg, 0);
    lv_obj_set_style_border_color(qrCard, theme::Line, 0);
    lv_obj_set_style_pad_all(qrCard, 8, 0);
    lv_obj_set_flex_flow(qrCard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(qrCard, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(qrCard, LV_OBJ_FLAG_SCROLLABLE);

    const lv_coord_t qrPx = cfg::UI_QR_SIDE - 20;
    g_qr = lv_qrcode_create(qrCard, qrPx, theme::QrFg, theme::QrBg);
    lv_qrcode_update(g_qr, payload, strlen(payload));

    lv_obj_t* side = lv_obj_create(root);
    lv_obj_set_flex_grow(side, 1);
    lv_obj_set_height(side, lv_pct(100));
    theme::styleTransparent(side);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(side, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(side, 10, 0);
    lv_obj_clear_flag(side, LV_OBJ_FLAG_SCROLLABLE);

    button(side, "< BACK", closeCb);
    lv_obj_t* head = heading(side, title);
    lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_LEFT, 0);
    if (successTitle) {
        lv_obj_set_style_text_color(head, theme::Success, 0);
    }

    if (hint && hint[0]) {
        lv_obj_t* hintLbl = fine(side, hint);
        lv_obj_set_style_text_align(hintLbl, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(hintLbl, theme::Muted, 0);
    }

    lv_obj_t* spacer = lv_obj_create(side);
    lv_obj_set_width(spacer, lv_pct(100));
    lv_obj_set_height(spacer, 1);
    theme::styleTransparent(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);

    if (footer && footer[0]) {
        lv_obj_t* footBox = lv_obj_create(side);
        lv_obj_set_width(footBox, lv_pct(100));
        lv_obj_set_height(footBox, LV_SIZE_CONTENT);
        theme::stylePanel(footBox);
        lv_obj_set_style_bg_color(footBox, theme::SurfaceHi, 0);
        lv_obj_set_style_pad_all(footBox, 10, 0);
        lv_obj_clear_flag(footBox, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* footLbl = lv_label_create(footBox);
        lv_label_set_text(footLbl, footer);
        lv_obj_set_width(footLbl, lv_pct(100));
        lv_label_set_long_mode(footLbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(footLbl, LV_TEXT_ALIGN_LEFT, 0);
        theme::applyBodySm(footLbl);
        lv_obj_set_style_text_color(footLbl, theme::Silver, 0);
    }
}

lv_obj_t* makeText(lv_obj_t* parent, const char* text) {
    lv_obj_t* obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_width(obj, lv_pct(100));
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    return obj;
}

lv_obj_t* title(lv_obj_t* parent, const char* text) {
    lv_obj_t* l = makeText(parent, text);
    theme::applyTitle(l);
    return l;
}

lv_obj_t* heading(lv_obj_t* parent, const char* text) {
    lv_obj_t* l = makeText(parent, text);
    theme::applyHeading(l);
    return l;
}

lv_obj_t* body(lv_obj_t* parent, const char* text) {
    lv_obj_t* l = makeText(parent, text);
    theme::applyBody(l);
    return l;
}

lv_obj_t* fine(lv_obj_t* parent, const char* text) {
    lv_obj_t* l = makeText(parent, text);
    theme::applyBodySm(l);
    return l;
}

namespace {

lv_color_t btnTextColor(Btn v) {
    switch (v) {
        case Btn::Primary:
        case Btn::Accent:
            return theme::Void;
        case Btn::Danger:
            return theme::Danger;
        default:
            return theme::Text;
    }
}

}  // namespace

lv_obj_t* button(lv_obj_t* parent, const char* text, lv_event_cb_t cb, Btn variant,
                 lv_event_code_t code) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, cfg::UI_BTN_H);
    switch (variant) {
        case Btn::Primary: theme::styleBtnPrimary(btn); break;
        case Btn::Accent: theme::styleBtnAccent(btn); break;
        case Btn::Danger: theme::styleBtnDanger(btn); break;
        default: theme::styleBtnSecondary(btn); break;
    }
    if (cb) lv_obj_add_event_cb(btn, cb, code, nullptr);
    lv_obj_t* l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &faraday_rajdhani_14, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_style_text_color(l, btnTextColor(variant), 0);
    lv_obj_center(l);
    return btn;
}

void route() {
    if (!wallet::hasWallet()) showOnboarding();
    else if (wallet::isUnlocked()) showHome();
    else showPinUnlock();
}

namespace {

struct IntroSlide {
    const char* kicker;
    const char* title;
    const char* body;
};

constexpr IntroSlide kIntro[] = {
    {"WHAT IS THIS", "AIR-GAPPED",
     "FARADAY is an offline XRPL signer. No WiFi, no radios - your keys never touch a network."},
    {"HOW IT WORKS", "REVIEW & SIGN",
     "Receive an unsigned transaction, inspect every field on-device, then hold to sign it."},
    {"STAYS SEALED", "USB IN, USB OUT",
     "Only the signed transaction leaves over USB. Your seed is sealed and never exits."},
};
constexpr int kIntroCount = sizeof(kIntro) / sizeof(kIntro[0]);

void introDots(lv_obj_t* parent, int active) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    theme::styleTransparent(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 7, 0);
    for (int i = 0; i < kIntroCount; ++i) {
        lv_obj_t* dot = lv_obj_create(row);
        const bool on = (i == active);
        lv_obj_set_size(dot, on ? 18 : 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, on ? theme::Accent : theme::Line, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    }
}

void animOpa(void* obj, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), 0);
}

void fadeIn(lv_obj_t* obj, uint32_t delay, uint32_t time) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a, animOpa);
    lv_anim_set_time(&a, time);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void splashDone(lv_timer_t* t) {
    lv_timer_del(t);
    g_splashDone = true;
    if (introSeen()) finishIntro();
    else showIntro(0);
}

}  // namespace

void finishIntro() {
    markIntroSeen(true);
    route();
}

void showIntro(int step) {
    if (step < 0) step = 0;
    if (step >= kIntroCount) {
        finishIntro();
        return;
    }
    g_introStep = step;
    const IntroSlide& slide = kIntro[step];

    lv_obj_t* p = panel();
    lv_obj_set_style_pad_all(p, cfg::UI_PAD, 0);

    lv_obj_t* kicker = makeText(p, slide.kicker);
    theme::applyLabel(kicker);
    lv_obj_set_style_text_color(kicker, theme::Accent, 0);

    title(p, slide.title);

    lv_obj_t* bodyLabel = body(p, slide.body);
    lv_obj_set_style_text_color(bodyLabel, theme::Silver, 0);

    lv_obj_t* spacer = lv_obj_create(p);
    lv_obj_set_width(spacer, lv_pct(100));
    lv_obj_set_height(spacer, 1);
    theme::styleTransparent(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);

    introDots(p, step);

    lv_obj_t* row = lv_obj_create(p);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    theme::styleTransparent(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    const bool last = (step == kIntroCount - 1);
    lv_event_cb_t leftCb = (step > 0)
        ? static_cast<lv_event_cb_t>([](lv_event_t*) { showIntro(g_introStep - 1); })
        : static_cast<lv_event_cb_t>([](lv_event_t*) { finishIntro(); });
    lv_obj_t* left = button(row, step > 0 ? "BACK" : "SKIP INTRO", leftCb);
    lv_obj_set_flex_grow(left, 1);

    lv_event_cb_t rightCb = last
        ? static_cast<lv_event_cb_t>([](lv_event_t*) { finishIntro(); })
        : static_cast<lv_event_cb_t>([](lv_event_t*) { showIntro(g_introStep + 1); });
    lv_obj_t* right = button(row, last ? "GET STARTED" : "NEXT", rightCb, Btn::Accent);
    lv_obj_set_flex_grow(right, 1);
}

}  // namespace internal

void build() {
    internal::clear();
}

void playSplash() {
    using namespace internal;
    clear();

    lv_obj_t* logo = lv_img_create(g_screen);
    lv_img_set_src(logo, &faraday_logo);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -18);
    lv_obj_set_style_opa(logo, LV_OPA_TRANSP, 0);

    lv_obj_t* tagline = lv_label_create(g_screen);
    lv_label_set_text(tagline, "YOUR KEYS. OFFLINE. SECURE.");
    lv_obj_set_style_text_font(tagline, &faraday_rajdhani_14, 0);
    lv_obj_set_style_text_color(tagline, theme::Accent, 0);
    lv_obj_set_style_text_letter_space(tagline, 3, 0);
    lv_obj_align(tagline, LV_ALIGN_CENTER, 0, 32);
    lv_obj_set_style_opa(tagline, LV_OPA_TRANSP, 0);

    lv_obj_t* spinner = lv_spinner_create(g_screen, 1100, 55);
    lv_obj_set_size(spinner, 30, 30);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 72);
    lv_obj_clear_flag(spinner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(spinner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_color(spinner, theme::Line, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, theme::Accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(spinner, true, LV_PART_INDICATOR);

    lv_obj_t* ver = lv_label_create(g_screen);
    lv_label_set_text(ver, cfg::FW_VERSION);
    lv_obj_set_style_text_font(ver, &faraday_inter_12, 0);
    lv_obj_set_style_text_color(ver, theme::Muted, 0);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_opa(ver, LV_OPA_TRANSP, 0);

    fadeIn(logo, 80, 520);
    fadeIn(tagline, 360, 420);
    fadeIn(spinner, 560, 360);
    fadeIn(ver, 700, 360);

    lv_timer_t* timer = lv_timer_create(splashDone, 2000, nullptr);
    lv_timer_set_repeat_count(timer, 1);
}

void tick() {
    using namespace internal;
    if (!g_splashDone) return;

    if (g_homeConnectDot && g_homeConnectLabel) {
        const bool active = usblink::hostActive();
        lv_obj_set_style_bg_color(g_homeConnectDot, active ? theme::Success : theme::Danger, 0);
        const char* want = active ? "Connected" : "Not connected";
        const char* have = lv_label_get_text(g_homeConnectLabel);
        if (!have || strcmp(have, want) != 0) {
            lv_label_set_text(g_homeConnectLabel, want);
            lv_obj_set_style_text_color(g_homeConnectLabel, active ? theme::Success : theme::Muted,
                                        0);
        }
    }

    if (wallet::isUnlocked() && lv_disp_get_inactive_time(nullptr) > cfg::AUTO_LOCK_MS) {
        wallet::lock();
        showPinUnlock();
    }

    processPendingSign();
}

}  // namespace ui
