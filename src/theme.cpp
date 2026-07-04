#include "theme.h"

#include "config.h"
#include "fonts.h"

namespace theme {

const lv_color_t Void = lv_color_hex(0x020305);
const lv_color_t Surface = lv_color_hex(0x0B121C);
const lv_color_t SurfaceHi = lv_color_hex(0x111827);
const lv_color_t Line = lv_color_hex(0x374151);
const lv_color_t Muted = lv_color_hex(0x6B7280);
const lv_color_t Silver = lv_color_hex(0xD9DEE7);
const lv_color_t Text = lv_color_hex(0xF5F7FA);

const lv_color_t Accent = lv_color_hex(0x00E5FF);
const lv_color_t AccentDim = lv_color_hex(0x065863);
const lv_color_t Info = lv_color_hex(0x2563FF);
const lv_color_t Danger = lv_color_hex(0xFF3B3B);
const lv_color_t Success = lv_color_hex(0x38FF88);

const lv_color_t QrFg = lv_color_hex(0x000000);
const lv_color_t QrBg = lv_color_hex(0xFFFFFF);

namespace {
constexpr int RADIUS = 10;
}

void init() {
    lv_disp_t* disp = lv_disp_get_default();
    lv_theme_t* th = lv_theme_default_init(disp, Accent, Info, true, &faraday_inter_14);
    lv_disp_set_theme(disp, th);
    lv_disp_set_bg_color(disp, Void);
    lv_disp_set_bg_opa(disp, LV_OPA_COVER);
}

void styleVoid(lv_obj_t* obj) {
    lv_obj_set_style_bg_color(obj, Void, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void styleTransparent(lv_obj_t* obj) {
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void stylePanel(lv_obj_t* obj) {
    lv_obj_set_style_bg_color(obj, Surface, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, Line, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, RADIUS, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

namespace {
void applyFont(lv_obj_t* label, const lv_font_t* font, lv_color_t color, lv_coord_t letterSpace) {
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_letter_space(label, letterSpace, 0);
}
}

void applyTitle(lv_obj_t* label) { applyFont(label, &faraday_orbitron_20, Text, 2); }
void applyHeading(lv_obj_t* label) { applyFont(label, &faraday_rajdhani_18, Text, 3); }
void applyLabel(lv_obj_t* label) { applyFont(label, &faraday_rajdhani_14, Muted, 2); }
void applyBody(lv_obj_t* label) { applyFont(label, &faraday_inter_14, Silver, 0); }
void applyBodySm(lv_obj_t* label) { applyFont(label, &faraday_inter_12, Muted, 0); }
void applyAmount(lv_obj_t* label) { applyFont(label, &faraday_inter_16, Text, 0); }

namespace {
void baseButton(lv_obj_t* btn) {
    lv_obj_set_style_radius(btn, RADIUS, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_outline_width(btn, 0, 0);
    lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);
}
}

void styleBtnPrimary(lv_obj_t* btn) {
    baseButton(btn);
    lv_obj_set_style_bg_color(btn, Text, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, Silver, LV_STATE_PRESSED);
}

void styleBtnSecondary(lv_obj_t* btn) {
    baseButton(btn);
    lv_obj_set_style_bg_color(btn, Surface, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, SurfaceHi, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, Line, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, Accent, LV_STATE_PRESSED);
}

void styleBtnAccent(lv_obj_t* btn) {
    baseButton(btn);
    lv_obj_set_style_bg_color(btn, Accent, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, AccentDim, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(btn, Accent, 0);
    lv_obj_set_style_shadow_width(btn, 14, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_shadow_spread(btn, 0, 0);
}

void styleBtnDanger(lv_obj_t* btn) {
    baseButton(btn);
    lv_obj_set_style_bg_color(btn, Surface, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, Danger, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, Danger, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
}

lv_obj_t* makeAccentBar(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, cfg::SCREEN_WIDTH, 2);
    lv_obj_set_style_bg_color(bar, Accent, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_shadow_color(bar, Accent, 0);
    lv_obj_set_style_shadow_width(bar, 8, 0);
    lv_obj_set_style_shadow_opa(bar, LV_OPA_30, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    return bar;
}

}  // namespace theme
