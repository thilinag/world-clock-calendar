/*
 * World Clock — 4 timezones + "compare a time" slider
 * Board : Waveshare ESP32-S3-Touch-AMOLED-2.16 (CO5300 480x480 QSPI, CST9220 touch, PCF85063 RTC)
 * Stack : LVGL 8.4 + Arduino_GFX 1.6.4 + SensorLib 0.3.3 (Waveshare bundled versions)
 *
 * Time source: NTP over WiFi when available; the UTC time is saved to the PCF85063 RTC,
 * so the clock keeps working after a reboot without WiFi.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>
#include <lvgl.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include "TouchDrvCSTXXX.hpp"
#include "SensorPCF85063.hpp"

// ---------------------------------------------------------------- user config
#include "secrets.h"   // WiFi credentials (copy secrets.example.h -> secrets.h)
static const char *WIFI_SSID = WIFI_SSID_VALUE;
static const char *WIFI_PASS = WIFI_PASS_VALUE;

// Daylight-saving rule sets (computed in the sketch; the C library's TZ code leaks memory)
enum DstRule {
  RULE_NONE,
  RULE_AU,   // Australia (VIC/SA/NSW/TAS/ACT): 1st Sun Oct 02:00 -> 1st Sun Apr 03:00
  RULE_US,   // USA/Canada:                     2nd Sun Mar 02:00 -> 1st Sun Nov 02:00
};

struct Zone {
  const char *name;
  int stdOffsetMin;      // standard-time offset from UTC in minutes
  DstRule dst;           // +60 min while daylight saving is active
  const char *stdAbbrev; // badge text if non-null (e.g. "PST"), otherwise "GMT+11" style
  const char *dstAbbrev;
  uint32_t accent;
};

static const Zone ZONES[4] = {
  { "Melbourne",   10 * 60,      RULE_AU,   nullptr, nullptr, 0xFBBF24 },
  { "Dreamscythe", -8 * 60,      RULE_US,   "PST",   "PDT",   0xA78BFA },
  { "Adelaide",    9 * 60 + 30,  RULE_AU,   nullptr, nullptr, 0x38BDF8 },
  { "Sri Lanka",   5 * 60 + 30,  RULE_NONE, nullptr, nullptr, 0x34D399 },
};

static const uint32_t SCREEN_TIMEOUT_MS = 2UL * 60UL * 1000UL;  // screen off after 2 min idle
static const uint8_t SCREEN_BRIGHTNESS = 180;
static const int WAKE_BUTTONS[] = { 18 /* KEY3 user button */, 0 /* BOOT */ };

static const int SLIDER_STEP_MIN = 15;                  // slider resolution
static const int SLIDER_RANGE = 12 * 60 / SLIDER_STEP_MIN;  // +/-12h

// ---------------------------------------------------------------- palette
#define C_BG        lv_color_hex(0x020617)
#define C_CARD      lv_color_hex(0x0F172A)
#define C_BORDER    lv_color_hex(0x1E293B)
#define C_TEXT      lv_color_hex(0xF1F5F9)
#define C_MUTED     lv_color_hex(0x94A3B8)
#define C_DIM       lv_color_hex(0x64748B)
#define C_BADGE_TXT lv_color_hex(0xCBD5E1)
#define C_PURPLE    lv_color_hex(0x8B5CF6)
#define C_TRACK     lv_color_hex(0xF1F5F9)

// ---------------------------------------------------------------- hardware
Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
TouchDrvCST92xx touch;
SensorPCF85063 rtc;
bool rtcOk = false;

volatile bool touchIrq = false;
void IRAM_ATTR onTouchIrq() { touchIrq = true; }

volatile bool ntpSynced = false;
void onNtpSync(struct timeval *) { ntpSynced = true; }

// ---------------------------------------------------------------- screen power
static bool screenOn = true;
static uint32_t lastActivity = 0;

static void screenWake() {
  lastActivity = millis();
  if (screenOn) return;
  screenOn = true;
  gfx->displayOn();
  updateClocks();                       // catch up on the time missed while off
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(nullptr);                 // redraw before lighting the panel
  gfx->setBrightness(SCREEN_BRIGHTNESS);
  Serial.println("Screen on");
}

static void screenSleep() {
  if (!screenOn) return;
  screenOn = false;
  gfx->setBrightness(0);
  gfx->displayOff();
  Serial.println("Screen off");
}

// Edge-triggered: reacts to a button changing state, so a pin that idles LOW
// (or a button that is held) can't keep the screen on forever.
static const int NUM_WAKE_BUTTONS = sizeof(WAKE_BUTTONS) / sizeof(WAKE_BUTTONS[0]);
static int btnLevel[NUM_WAKE_BUTTONS];
static uint32_t btnChangedAt[NUM_WAKE_BUTTONS];

static bool wakeButtonPressed() {
  bool pressed = false;
  for (int i = 0; i < NUM_WAKE_BUTTONS; i++) {
    int lvl = digitalRead(WAKE_BUTTONS[i]);
    if (lvl != btnLevel[i] && millis() - btnChangedAt[i] > 50) {   // 50 ms debounce
      btnLevel[i] = lvl;
      btnChangedAt[i] = millis();
      Serial.printf("Button GPIO%d -> %s\n", WAKE_BUTTONS[i], lvl ? "HIGH" : "LOW");
      pressed = true;
    }
  }
  return pressed;
}

// ---------------------------------------------------------------- UI objects
struct CardUI {
  lv_obj_t *time;
  lv_obj_t *date;
  lv_obj_t *badge;
  lv_obj_t *badgeLabel;
};
static CardUI cards[4];
static lv_obj_t *slider;
static lv_obj_t *offsetLabel;
static lv_obj_t *resetBtn;
static lv_obj_t *resetLabel;
static int sliderSteps = 0;   // offset in SLIDER_STEP_MIN units

// ---------------------------------------------------------------- LVGL glue
static lv_disp_draw_buf_t drawBuf;
static lv_disp_drv_t dispDrv;
static lv_indev_drv_t indevDrv;

static void dispFlush(lv_disp_drv_t *disp, const lv_area_t *a, lv_color_t *px) {
  uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(a->x1, a->y1, (uint16_t *)&px->full, w, h);
#else
  gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)&px->full, w, h);
#endif
  lv_disp_flush_ready(disp);
}

// CO5300 requires even start / odd end coordinates
static void dispRounder(lv_disp_drv_t *, lv_area_t *a) {
  if (a->x1 & 1) a->x1--;
  if (a->y1 & 1) a->y1--;
  if (!(a->x2 & 1)) a->x2++;
  if (!(a->y2 & 1)) a->y2++;
}

static void touchRead(lv_indev_drv_t *, lv_indev_data_t *data) {
  static bool pressed = false;
  static int16_t lastX = 0, lastY = 0;
  // Read on interrupt, and keep polling while a finger is down so drags are smooth.
  if (touchIrq || pressed) {
    touchIrq = false;
    int16_t x[2], y[2];
    uint8_t n = touch.getPoint(x, y, touch.getSupportTouchPoint());
    pressed = n > 0;
    if (pressed) { lastX = x[0]; lastY = y[0]; }
  }
  // Ignore touches while the screen is off (so the slider can't move blind).
  if (!screenOn) pressed = false;
  if (pressed) {                          // using the UI keeps the screen on
    lastActivity = millis();
    static uint32_t lastLog = 0;
    if (millis() - lastLog > 2000) { lastLog = millis(); Serial.printf("Touch at %d,%d\n", lastX, lastY); }
  }
  data->point.x = lastX;
  data->point.y = lastY;
  data->state = pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

static void lvTick(void *) { lv_tick_inc(2); }

// ---------------------------------------------------------------- time helpers
static bool timeValid() { return time(nullptr) > 1700000000; }  // after Nov 2023

// Timezone maths is done here with gmtime_r() only. Switching the TZ env var
// (setenv/tzset) several times a second leaked heap in the ESP32 C library
// and crashed the board after ~2 minutes.

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Seconds (in the zone's standard-time scale) at which the n-th Sunday of a month hits `hour`.
static int64_t nthSundayAt(int year, int month, int n, int hour) {
  int64_t first = daysFromCivil(year, month, 1);
  int dow = (int)((first + 4) % 7);            // 1970-01-01 was a Thursday; 0 = Sunday
  int64_t day = first + (7 - dow) % 7 + 7 * (n - 1);
  return day * 86400 + hour * 3600;
}

static bool isDst(const Zone &z, time_t utc) {
  if (z.dst == RULE_NONE) return false;
  int64_t ls = (int64_t)utc + z.stdOffsetMin * 60;   // local standard time
  struct tm st;
  time_t lsT = (time_t)ls;
  gmtime_r(&lsT, &st);
  int y = st.tm_year + 1900;
  if (z.dst == RULE_AU) {
    // starts 02:00 std; ends 03:00 daylight = 02:00 std
    return ls >= nthSundayAt(y, 10, 1, 2) || ls < nthSundayAt(y, 4, 1, 2);
  }
  // RULE_US: starts 02:00 std; ends 02:00 daylight = 01:00 std
  return ls >= nthSundayAt(y, 3, 2, 2) && ls < nthSundayAt(y, 11, 1, 1);
}

// Local time in a zone; returns the zone's current UTC offset in seconds.
static int localTimeIn(const Zone &z, time_t utc, struct tm *out, bool *dst) {
  *dst = isDst(z, utc);
  int off = (z.stdOffsetMin + (*dst ? 60 : 0)) * 60;
  time_t lt = utc + off;
  gmtime_r(&lt, out);
  return off;
}

static void formatGmt(int offSec, char *buf, size_t n) {
  if (offSec == 0) { snprintf(buf, n, "GMT"); return; }
  char sign = offSec < 0 ? '-' : '+';
  int a = abs(offSec) / 60;
  if (a % 60) snprintf(buf, n, "GMT%c%d:%02d", sign, a / 60, a % 60);
  else        snprintf(buf, n, "GMT%c%d", sign, a / 60);
}

static void setTextIfChanged(lv_obj_t *label, const char *txt) {
  if (strcmp(lv_label_get_text(label), txt) != 0) lv_label_set_text(label, txt);
}

static void rtcToSystem() {
  if (!rtcOk) return;
  RTC_DateTime d = rtc.getDateTime();              // RTC holds UTC
  time_t epoch = (time_t)(daysFromCivil(d.getYear(), d.getMonth(), d.getDay()) * 86400 +
                          d.getHour() * 3600 + d.getMinute() * 60 + d.getSecond());
  if (epoch > 1700000000) {
    struct timeval tv = { epoch, 0 };
    settimeofday(&tv, nullptr);
    Serial.println("Time restored from RTC");
  }
}

static void systemToRtc() {
  if (!rtcOk) return;
  time_t now = time(nullptr);
  struct tm utc;
  gmtime_r(&now, &utc);
  rtc.setDateTime(RTC_DateTime(utc));
  Serial.println("RTC updated from NTP");
}

// ---------------------------------------------------------------- UI update
static void updateClocks() {
  static const char *DAYS[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
  static const char *MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

  bool valid = timeValid();
  time_t t = time(nullptr) + (time_t)sliderSteps * SLIDER_STEP_MIN * 60;

  for (int i = 0; i < 4; i++) {
    if (!valid) {
      setTextIfChanged(cards[i].time, "--:--");
      setTextIfChanged(cards[i].date, "Waiting for time sync...");
      continue;
    }
    const Zone &z = ZONES[i];
    struct tm lt;
    bool dst;
    int offSec = localTimeIn(z, t, &lt, &dst);

    char buf[40];
    int h12 = lt.tm_hour % 12;
    if (h12 == 0) h12 = 12;
    snprintf(buf, sizeof(buf), "%d:%02d %s", h12, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
    setTextIfChanged(cards[i].time, buf);

    snprintf(buf, sizeof(buf), "%s, %s %d, %d", DAYS[lt.tm_wday], MONTHS[lt.tm_mon], lt.tm_mday, lt.tm_year + 1900);
    setTextIfChanged(cards[i].date, buf);

    if (z.stdAbbrev) {
      snprintf(buf, sizeof(buf), "%s", dst ? z.dstAbbrev : z.stdAbbrev);
    } else {
      formatGmt(offSec, buf, sizeof(buf));
    }
    setTextIfChanged(cards[i].badgeLabel, buf);
  }
}

static void updateOffsetUi() {
  if (sliderSteps == 0) {
    lv_label_set_text(offsetLabel, "");
    lv_obj_add_state(resetBtn, LV_STATE_DISABLED);
  } else {
    int m = abs(sliderSteps) * SLIDER_STEP_MIN;
    char buf[24];
    if (m % 60) snprintf(buf, sizeof(buf), "%c%dh %02dm", sliderSteps > 0 ? '+' : '-', m / 60, m % 60);
    else        snprintf(buf, sizeof(buf), "%c%dh", sliderSteps > 0 ? '+' : '-', m / 60);
    lv_label_set_text(offsetLabel, buf);
    lv_obj_clear_state(resetBtn, LV_STATE_DISABLED);
  }
  updateClocks();
}

static void onSlider(lv_event_t *e) {
  sliderSteps = lv_slider_get_value(slider);
  updateOffsetUi();
}

static void onReset(lv_event_t *e) {
  sliderSteps = 0;
  lv_slider_set_value(slider, 0, LV_ANIM_ON);
  updateOffsetUi();
}

// ---------------------------------------------------------------- UI build
static lv_obj_t *makePanel(lv_obj_t *parent, int x, int y, int w, int h) {
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

static lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, txt);
  return l;
}

static void buildUi() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, C_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  const int M = 14, G = 12;
  const int W = LCD_WIDTH - 2 * M;

  // ---- "Compare a time" panel
  const int topH = 98;
  lv_obj_t *top = makePanel(scr, M, M, W, topH);

  lv_obj_t *title = makeLabel(top, &lv_font_montserrat_16, C_TEXT, "Compare a time");
  lv_obj_set_pos(title, 18, 20);

  offsetLabel = makeLabel(top, &lv_font_montserrat_16, lv_color_hex(0xA78BFA), "");
  lv_obj_align_to(offsetLabel, title, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

  resetBtn = lv_btn_create(top);
  lv_obj_remove_style_all(resetBtn);
  lv_obj_set_size(resetBtn, 118, 34);
  lv_obj_align(resetBtn, LV_ALIGN_TOP_RIGHT, -14, 12);
  lv_obj_set_style_radius(resetBtn, 17, 0);
  lv_obj_set_style_border_width(resetBtn, 1, 0);
  lv_obj_set_style_border_color(resetBtn, C_PURPLE, 0);
  lv_obj_set_style_border_color(resetBtn, C_BORDER, LV_STATE_DISABLED);
  lv_obj_set_style_bg_color(resetBtn, C_PURPLE, 0);
  lv_obj_set_style_bg_opa(resetBtn, LV_OPA_20, 0);
  lv_obj_set_style_bg_opa(resetBtn, LV_OPA_50, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(resetBtn, LV_OPA_TRANSP, LV_STATE_DISABLED);
  lv_obj_set_style_text_color(resetBtn, C_TEXT, 0);
  lv_obj_set_style_text_color(resetBtn, C_DIM, LV_STATE_DISABLED);
  lv_obj_set_ext_click_area(resetBtn, 8);
  lv_obj_add_event_cb(resetBtn, onReset, LV_EVENT_CLICKED, nullptr);
  resetLabel = makeLabel(resetBtn, &lv_font_montserrat_14, C_TEXT, "Reset to now");
  lv_obj_center(resetLabel);
  // let the label inherit the (disabled/enabled) text colour from the button
  lv_obj_remove_local_style_prop(resetLabel, LV_STYLE_TEXT_COLOR, 0);

  lv_obj_t *minL = makeLabel(top, &lv_font_montserrat_14, C_DIM, "-12h");
  lv_obj_align(minL, LV_ALIGN_BOTTOM_LEFT, 18, -18);
  lv_obj_t *maxL = makeLabel(top, &lv_font_montserrat_14, C_DIM, "+12h");
  lv_obj_align(maxL, LV_ALIGN_BOTTOM_RIGHT, -18, -18);

  slider = lv_slider_create(top);
  lv_obj_remove_style_all(slider);
  lv_obj_set_size(slider, W - 2 * 18 - 2 * 44, 8);
  lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -22);
  lv_slider_set_range(slider, -SLIDER_RANGE, SLIDER_RANGE);
  lv_slider_set_value(slider, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider, C_TRACK, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, C_PURPLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, C_PURPLE, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 5, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB | LV_STATE_PRESSED);
  lv_obj_set_ext_click_area(slider, 20);
  lv_obj_add_event_cb(slider, onSlider, LV_EVENT_VALUE_CHANGED, nullptr);

  // ---- 2x2 timezone cards
  const int y0 = M + topH + G;
  const int cw = (W - G) / 2;
  const int ch = (LCD_HEIGHT - M - y0 - G) / 2;

  for (int i = 0; i < 4; i++) {
    int cx = M + (i % 2) * (cw + G);
    int cy = y0 + (i / 2) * (ch + G);
    lv_obj_t *card = makePanel(scr, cx, cy, cw, ch);
    lv_obj_set_style_clip_corner(card, true, 0);

    lv_obj_t *bar = lv_obj_create(card);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, cw, 5);
    lv_obj_set_pos(bar, -1, -1);
    lv_obj_set_style_bg_color(bar, lv_color_hex(ZONES[i].accent), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    lv_obj_t *name = makeLabel(card, &lv_font_montserrat_18, C_TEXT, ZONES[i].name);
    lv_obj_set_pos(name, 16, 24);

    cards[i].badge = lv_obj_create(card);
    lv_obj_remove_style_all(cards[i].badge);
    lv_obj_set_size(cards[i].badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(cards[i].badge, C_BORDER, 0);
    lv_obj_set_style_bg_opa(cards[i].badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cards[i].badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(cards[i].badge, 9, 0);
    lv_obj_set_style_pad_ver(cards[i].badge, 5, 0);
    lv_obj_align(cards[i].badge, LV_ALIGN_TOP_RIGHT, -12, 20);
    cards[i].badgeLabel = makeLabel(cards[i].badge, &lv_font_montserrat_12, C_BADGE_TXT, "GMT");

    cards[i].time = makeLabel(card, &lv_font_montserrat_40, C_TEXT, "--:--");
    lv_obj_set_pos(cards[i].time, 14, 64);

    cards[i].date = makeLabel(card, &lv_font_montserrat_14, C_MUTED, "");
    lv_obj_align(cards[i].date, LV_ALIGN_BOTTOM_LEFT, 16, -18);
  }

  updateOffsetUi();
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);

  Wire.begin(IIC_SDA, IIC_SCL);

  // Display
  if (!gfx->begin()) Serial.println("gfx->begin() failed!");
  bus->writeC8D8(0x36, 0xA0);
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(SCREEN_BRIGHTNESS);

  for (int i = 0; i < NUM_WAKE_BUTTONS; i++) {
    pinMode(WAKE_BUTTONS[i], INPUT_PULLUP);
    btnLevel[i] = digitalRead(WAKE_BUTTONS[i]);
    Serial.printf("Button GPIO%d idle level: %s\n", WAKE_BUTTONS[i], btnLevel[i] ? "HIGH" : "LOW");
  }

  // Touch
  touch.setPins(TP_RST, TP_INT);
  if (touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setSwapXY(true);
    touch.setMirrorXY(true, false);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), onTouchIrq, FALLING);
  } else {
    Serial.println("Touch not found");
  }

  // RTC -> system clock
  rtcOk = rtc.begin(Wire, IIC_SDA, IIC_SCL);
  if (!rtcOk) Serial.println("PCF85063 not found");
  rtcToSystem();

  // NTP (non-blocking; RTC keeps time meanwhile)
  sntp_set_time_sync_notification_cb(onNtpSync);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  // LVGL
  lv_init();
  const size_t bufPx = LCD_WIDTH * 48;
  lv_color_t *b1 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_color_t *b2 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_disp_draw_buf_init(&drawBuf, b1, b2, bufPx);

  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = LCD_WIDTH;
  dispDrv.ver_res = LCD_HEIGHT;
  dispDrv.flush_cb = dispFlush;
  dispDrv.rounder_cb = dispRounder;
  dispDrv.draw_buf = &drawBuf;
  lv_disp_drv_register(&dispDrv);

  lv_indev_drv_init(&indevDrv);
  indevDrv.type = LV_INDEV_TYPE_POINTER;
  indevDrv.read_cb = touchRead;
  lv_indev_drv_register(&indevDrv);

  const esp_timer_create_args_t tickArgs = { .callback = &lvTick, .name = "lv_tick" };
  esp_timer_handle_t tickTimer;
  esp_timer_create(&tickArgs, &tickTimer);
  esp_timer_start_periodic(tickTimer, 2000);

  buildUi();
  lastActivity = millis();
}

void loop() {
  static uint32_t lastUpdate = 0;

  // Screen timeout: a button press turns the screen on (or keeps it on) for another 5 minutes.
  if (wakeButtonPressed()) screenWake();
  if (screenOn && millis() - lastActivity >= SCREEN_TIMEOUT_MS) screenSleep();

  if (ntpSynced) {
    ntpSynced = false;
    systemToRtc();
  }

  if (!screenOn) {          // no drawing / panel traffic while the screen is off
    delay(20);
    return;
  }

  if (millis() - lastUpdate >= 250) {
    lastUpdate = millis();
    updateClocks();
    static uint32_t lastHeapLog = 0;
    if (millis() - lastHeapLog >= 30000) {
      lastHeapLog = millis();
      Serial.printf("Free heap: %u (min %u)\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
    }
  }

  lv_timer_handler();
  delay(5);
}
