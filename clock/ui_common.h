// Palette and small LVGL helpers shared by the clock and calendar pages.
#pragma once
#include <lvgl.h>
#include <string.h>

#define C_BG        lv_color_hex(0x020617)
#define C_CARD      lv_color_hex(0x0F172A)
#define C_BORDER    lv_color_hex(0x1E293B)
#define C_TEXT      lv_color_hex(0xF1F5F9)
#define C_MUTED     lv_color_hex(0x94A3B8)
#define C_DIM       lv_color_hex(0x64748B)
#define C_BADGE_TXT lv_color_hex(0xCBD5E1)
#define C_PURPLE    lv_color_hex(0x8B5CF6)
#define C_PURPLE_LT lv_color_hex(0xA78BFA)
#define C_TRACK     lv_color_hex(0xF1F5F9)
#define C_ALERT_RED lv_color_hex(0xDC2626)

static inline lv_obj_t *makePanel(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *p = lv_obj_create(parent);
  lv_obj_remove_style_all(p);
  lv_obj_set_pos(p, x, y);
  lv_obj_set_size(p, w, h);
  lv_obj_set_style_bg_color(p, C_CARD, 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(p, 18, 0);
  lv_obj_set_style_border_color(p, C_BORDER, 0);
  lv_obj_set_style_border_width(p, 1, 0);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  return p;
}

// Coloured strip along the top edge of a card (card needs clip_corner for the rounded look).
static inline lv_obj_t *makeAccentBar(lv_obj_t *card, int width, lv_color_t color) {
  lv_obj_set_style_clip_corner(card, true, 0);
  lv_obj_t *bar = lv_obj_create(card);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, width, 5);
  lv_obj_set_pos(bar, -1, -1);
  lv_obj_set_style_bg_color(bar, color, 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  return bar;
}

static inline lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, txt);
  return l;
}

// Rounded "pill" container with a label inside; returns the label.
static inline lv_obj_t *makePill(lv_obj_t *parent, lv_color_t bg, lv_opa_t bgOpa, const lv_font_t *font,
                                 lv_color_t textColor, lv_obj_t **pillOut = nullptr) {
  lv_obj_t *pill = lv_obj_create(parent);
  lv_obj_remove_style_all(pill);
  lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(pill, bg, 0);
  lv_obj_set_style_bg_opa(pill, bgOpa, 0);
  lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(pill, 10, 0);
  lv_obj_set_style_pad_ver(pill, 5, 0);
  lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
  if (pillOut) *pillOut = pill;
  return makeLabel(pill, font, textColor, "");
}

static inline void setTextIfChanged(lv_obj_t *label, const char *txt) {
  if (strcmp(lv_label_get_text(label), txt) != 0) lv_label_set_text(label, txt);
}
