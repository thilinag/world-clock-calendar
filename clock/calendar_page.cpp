#include "calendar_page.h"
#include "ui_common.h"
#include "countdown_font.h"
#include "pin_config.h"

static const Zone *home;

// page widgets
static lv_obj_t *statusLabel;
static lv_obj_t *relPill, *relLabel;
static lv_obj_t *titleLabel, *timeLabel, *detailLabel;
static lv_obj_t *page;

// "Later" list rows (events after the next one)
static const int LATER_ROWS = CAL_MAX_EVENTS - 1;
static const int ROW_H = 74, ROW_GAP = 10;
struct LaterRow { lv_obj_t *card, *title, *rel, *detail; };
static LaterRow later[LATER_ROWS];
static lv_obj_t *laterHeading;

// countdown overlay
static lv_obj_t *overlay, *cdSeconds, *cdTitle;
static lv_font_t *cdFont;

// ------------------------------------------------------------------ formatting
static int64_t localDay(time_t utc) {
  struct tm lt;
  int off = localTimeIn(*home, utc, &lt);
  return ((int64_t)utc + off) / 86400;
}

static void formatRelative(time_t start, time_t now, char *buf, size_t n) {
  long d = (long)(start - now);
  if (d < 60) { snprintf(buf, n, "starting now"); return; }
  if (d < 3600) { snprintf(buf, n, "in %ld min", (d + 59) / 60); return; }
  if (d < 12 * 3600) {
    long h = d / 3600, m = (d % 3600) / 60;
    if (m) snprintf(buf, n, "in %ld h %ld min", h, m);
    else   snprintf(buf, n, "in %ld h", h);
    return;
  }
  long days = (long)(localDay(start) - localDay(now));
  if (days <= 0)      snprintf(buf, n, "later today");
  else if (days == 1) snprintf(buf, n, "tomorrow");
  else                snprintf(buf, n, "in %ld days", days);
}

// "Wed, Nov 4  |  10:00 AM - 10:30 AM"
static void formatDetail(const CalEvent &e, char *buf, size_t n) {
  struct tm s, en;
  localTimeIn(*home, e.start, &s);
  char st[12], et[12];
  formatTime12(s, st, sizeof(st));
  if (e.end > e.start) {
    localTimeIn(*home, e.end, &en);
    formatTime12(en, et, sizeof(et));
    snprintf(buf, n, "%s, %s %d  |  %s - %s", DAY_SHORT[s.tm_wday], MONTH_SHORT[s.tm_mon], s.tm_mday, st, et);
  } else {
    snprintf(buf, n, "%s, %s %d  |  %s", DAY_SHORT[s.tm_wday], MONTH_SHORT[s.tm_mon], s.tm_mday, st);
  }
}

// ------------------------------------------------------------------ build
void calendarPageCreate(lv_obj_t *tile, const Zone *homeZone) {
  home = homeZone;
  const int M = 14, G = 12;
  const int W = LCD_WIDTH - 2 * M;

  // header
  lv_obj_t *head = makePanel(tile, M, M, W, 64);
  lv_obj_t *h = makeLabel(head, &lv_font_montserrat_20, C_TEXT, "Next event");
  lv_obj_align(h, LV_ALIGN_LEFT_MID, 18, 0);
  lv_obj_t *statusPill;
  statusLabel = makePill(head, C_BORDER, LV_OPA_COVER, &lv_font_montserrat_12, C_BADGE_TXT, &statusPill);
  lv_obj_align(statusPill, LV_ALIGN_RIGHT_MID, -14, 0);

  // main card
  const int mainY = M + 64 + G, mainH = 248;
  lv_obj_t *card = makePanel(tile, M, mainY, W, mainH);
  makeAccentBar(card, W, C_PURPLE);

  // big, high-contrast "in 23 min" so the time-to-event is readable from a distance
  relLabel = makePill(card, C_PURPLE, LV_OPA_COVER, &lv_font_montserrat_28, lv_color_white(), &relPill);
  lv_obj_set_style_pad_hor(relPill, 16, 0);
  lv_obj_set_style_pad_ver(relPill, 6, 0);
  lv_obj_set_pos(relPill, 16, 18);

  titleLabel = makeLabel(card, &lv_font_montserrat_28, C_TEXT, "");
  lv_obj_set_pos(titleLabel, 18, 72);
  lv_obj_set_width(titleLabel, W - 36);
  lv_obj_set_height(titleLabel, 66);                     // 2 lines max
  lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_DOT);

  timeLabel = makeLabel(card, &lv_font_montserrat_48, C_TEXT, "");
  lv_obj_set_pos(timeLabel, 16, 140);

  detailLabel = makeLabel(card, &lv_font_montserrat_16, C_MUTED, "");
  lv_obj_set_width(detailLabel, W - 36);
  lv_label_set_long_mode(detailLabel, LV_LABEL_LONG_DOT);
  lv_obj_align(detailLabel, LV_ALIGN_BOTTOM_LEFT, 18, -16);

  // "Later" list: the rest of the upcoming events; the page scrolls vertically to show them all.
  page = tile;
  lv_obj_set_scroll_dir(tile, LV_DIR_VER);                 // horizontal drags still switch pages
  lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_pad_bottom(tile, 22, 0);                // breathing room below the last row
  lv_obj_set_style_bg_color(tile, C_DIM, LV_PART_SCROLLBAR);

  int y = mainY + mainH + G;
  laterHeading = makeLabel(tile, &lv_font_montserrat_14, C_DIM, "Later");
  lv_obj_set_pos(laterHeading, M + 6, y + 2);
  y += 26;
  for (int i = 0; i < LATER_ROWS; i++) {
    LaterRow &r = later[i];
    r.card = makePanel(tile, M, y, W, ROW_H);
    r.title = makeLabel(r.card, &lv_font_montserrat_20, C_TEXT, "");
    lv_obj_set_pos(r.title, 18, 13);
    lv_obj_set_size(r.title, W - 36 - 110, 24);
    lv_label_set_long_mode(r.title, LV_LABEL_LONG_DOT);
    r.rel = makeLabel(r.card, &lv_font_montserrat_16, C_PURPLE_LT, "");
    lv_obj_align(r.rel, LV_ALIGN_TOP_RIGHT, -16, 15);
    r.detail = makeLabel(r.card, &lv_font_montserrat_14, C_MUTED, "");
    lv_obj_set_width(r.detail, W - 36);
    lv_label_set_long_mode(r.detail, LV_LABEL_LONG_DOT);
    lv_obj_align(r.detail, LV_ALIGN_BOTTOM_LEFT, 18, -13);
    lv_obj_add_flag(r.card, LV_OBJ_FLAG_HIDDEN);
    y += ROW_H + ROW_GAP;
  }

  // countdown overlay (top layer, covers both pages; absorbs touches)
  // Cache must be >= size*size bytes: LVGL 8.4 computes its LRU hash table as cache/(size*size),
  // and the default 4 KB cache gives 0 -> divide-by-zero crash. Room for ~12 glyphs (all digits); lives in PSRAM.
  const int cdSize = 230;
  cdFont = lv_tiny_ttf_create_data_ex(countdown_font_ttf, countdown_font_ttf_len, cdSize, (size_t)cdSize * cdSize * 12);
  overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(overlay);
  lv_obj_set_size(overlay, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_set_style_bg_color(overlay, C_ALERT_RED, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *startsIn = makeLabel(overlay, &lv_font_montserrat_24, lv_color_white(), "Starting in");
  lv_obj_set_style_text_opa(startsIn, LV_OPA_80, 0);
  lv_obj_align(startsIn, LV_ALIGN_TOP_MID, 0, 56);

  cdSeconds = makeLabel(overlay, cdFont ? cdFont : &lv_font_montserrat_48, lv_color_white(), "60");
  lv_obj_align(cdSeconds, LV_ALIGN_CENTER, 0, -6);

  cdTitle = makeLabel(overlay, &lv_font_montserrat_28, lv_color_white(), "");
  lv_obj_set_width(cdTitle, LCD_WIDTH - 60);
  lv_obj_set_style_text_align(cdTitle, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(cdTitle, LV_LABEL_LONG_DOT);
  lv_obj_set_height(cdTitle, 66);
  lv_obj_align(cdTitle, LV_ALIGN_BOTTOM_MID, 0, -44);

  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

// ------------------------------------------------------------------ update
static void setHidden(lv_obj_t *o, bool hidden) {
  if (hidden != lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
}

static void showLater(const CalEvent *const *events, int n, time_t now) {
  char buf[64];
  setHidden(laterHeading, n == 0);
  for (int i = 0; i < LATER_ROWS; i++) {
    LaterRow &r = later[i];
    setHidden(r.card, i >= n);
    if (i >= n) continue;
    setTextIfChanged(r.title, events[i]->title);
    formatRelative(events[i]->start, now, buf, sizeof(buf));
    setTextIfChanged(r.rel, buf);
    formatDetail(*events[i], buf, sizeof(buf));
    setTextIfChanged(r.detail, buf);
  }
}

void calendarPageScrollTop() {
  lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);
}

static void showMessage(const char *title, const char *detail) {
  lv_obj_add_flag(relPill, LV_OBJ_FLAG_HIDDEN);
  setTextIfChanged(titleLabel, title);
  setTextIfChanged(timeLabel, "");
  setTextIfChanged(detailLabel, detail);
  showLater(nullptr, 0, 0);
}

void calendarPageUpdate(time_t now, const CalendarState &st) {
  char buf[64];

  // status chip
  switch (st.status) {
    case CAL_OK: {
      struct tm lt;
      localTimeIn(*home, st.updatedAt, &lt);
      char t[12];
      formatTime12(lt, t, sizeof(t));
      snprintf(buf, sizeof(buf), "Updated %s", t);
      break;
    }
    case CAL_WAITING:        snprintf(buf, sizeof(buf), "Connecting"); break;
    case CAL_NOT_CONFIGURED: snprintf(buf, sizeof(buf), "Not set up"); break;
    case CAL_AUTH_ERROR:     snprintf(buf, sizeof(buf), "Sign-in expired"); break;
    case CAL_NET_ERROR:
      if (st.lastHttp > 0) snprintf(buf, sizeof(buf), "Error %d", st.lastHttp);
      else                 snprintf(buf, sizeof(buf), "Offline");
      break;
  }
  setTextIfChanged(statusLabel, buf);

  const CalEvent *up[CAL_MAX_EVENTS];
  int n = calendarUpcoming(st, now, up, CAL_MAX_EVENTS);

  if (n == 0) {
    switch (st.status) {
      case CAL_NOT_CONFIGURED: showMessage("Calendar not set up", "Add your Google keys to secrets.h"); break;
      case CAL_AUTH_ERROR:     showMessage("Google sign-in expired", "Re-run get_google_refresh_token.ps1"); break;
      case CAL_WAITING:        showMessage("Loading calendar...", ""); break;
      case CAL_NET_ERROR:      showMessage("Can't reach Google Calendar", "Will retry shortly"); break;
      case CAL_OK:             showMessage("No more events today", "Enjoy the rest of your day"); break;
    }
    return;
  }

  const CalEvent &e = *up[0];
  lv_obj_clear_flag(relPill, LV_OBJ_FLAG_HIDDEN);
  formatRelative(e.start, now, buf, sizeof(buf));
  setTextIfChanged(relLabel, buf);
  setTextIfChanged(titleLabel, e.title);

  struct tm s;
  localTimeIn(*home, e.start, &s);
  formatTime12(s, buf, sizeof(buf));
  setTextIfChanged(timeLabel, buf);

  formatDetail(e, buf, sizeof(buf));
  setTextIfChanged(detailLabel, buf);

  showLater(up + 1, n - 1, now);
}

// ------------------------------------------------------------------ countdown
void countdownShow(const char *title) {
  lv_label_set_text(cdTitle, title);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

void countdownSet(long seconds) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%ld", seconds);
  setTextIfChanged(cdSeconds, buf);
}

void countdownHide() {
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}
