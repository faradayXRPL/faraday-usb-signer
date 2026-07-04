#pragma once

#include <lvgl.h>

namespace theme {

extern const lv_color_t Void;
extern const lv_color_t Surface;
extern const lv_color_t SurfaceHi;
extern const lv_color_t Line;
extern const lv_color_t Muted;
extern const lv_color_t Silver;
extern const lv_color_t Text;

extern const lv_color_t Accent;
extern const lv_color_t AccentDim;
extern const lv_color_t Info;
extern const lv_color_t Danger;
extern const lv_color_t Success;

extern const lv_color_t QrFg;
extern const lv_color_t QrBg;

void init();

void styleVoid(lv_obj_t* obj);
void stylePanel(lv_obj_t* obj);
void styleTransparent(lv_obj_t* obj);

void applyTitle(lv_obj_t* label);
void applyHeading(lv_obj_t* label);
void applyLabel(lv_obj_t* label);
void applyBody(lv_obj_t* label);
void applyBodySm(lv_obj_t* label);
void applyAmount(lv_obj_t* label);

void styleBtnPrimary(lv_obj_t* btn);
void styleBtnSecondary(lv_obj_t* btn);
void styleBtnAccent(lv_obj_t* btn);
void styleBtnDanger(lv_obj_t* btn);

lv_obj_t* makeAccentBar(lv_obj_t* parent);

}  // namespace theme
