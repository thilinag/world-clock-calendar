// Timezone / date helpers shared by the clock and calendar pages.
//
// Timezone maths is done here with gmtime_r() only. Switching the TZ env var
// (setenv/tzset) several times a second leaked heap in the ESP32 C library
// and crashed the board after ~2 minutes.
#pragma once
#include <Arduino.h>
#include <time.h>
#include <stdlib.h>

// Daylight-saving rule sets
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

static const char *const DAY_NAMES[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const DAY_SHORT[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const MONTH_SHORT[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static inline bool timeValid() { return time(nullptr) > 1700000000; }  // after Nov 2023

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
static inline int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Seconds (in the zone's standard-time scale) at which the n-th Sunday of a month hits `hour`.
static inline int64_t nthSundayAt(int year, int month, int n, int hour) {
  int64_t first = daysFromCivil(year, month, 1);
  int dow = (int)((first + 4) % 7);            // 1970-01-01 was a Thursday; 0 = Sunday
  int64_t day = first + (7 - dow) % 7 + 7 * (n - 1);
  return day * 86400 + hour * 3600;
}

static inline bool isDst(const Zone &z, time_t utc) {
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
static inline int localTimeIn(const Zone &z, time_t utc, struct tm *out, bool *dst = nullptr) {
  bool d = isDst(z, utc);
  if (dst) *dst = d;
  int off = (z.stdOffsetMin + (d ? 60 : 0)) * 60;
  time_t lt = utc + off;
  gmtime_r(&lt, out);
  return off;
}

// "7:05 PM"
static inline void formatTime12(const struct tm &lt, char *buf, size_t n) {
  int h12 = lt.tm_hour % 12;
  if (h12 == 0) h12 = 12;
  snprintf(buf, n, "%d:%02d %s", h12, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
}

// Parse RFC 3339 ("2026-11-04T10:00:00+11:00", "...Z", "...T10:00:00.000-07:00") to UTC epoch.
// Returns 0 on failure.
static inline time_t parseRfc3339(const char *s) {
  if (!s) return 0;
  int Y, M, D, h, m, sec;
  if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  const char *p = s + 19;
  if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }   // fractional seconds
  int offSec = 0;
  if (*p == '+' || *p == '-') {
    int oh = 0, om = 0;
    if (sscanf(p + 1, "%2d:%2d", &oh, &om) < 1) return 0;
    offSec = (oh * 3600 + om * 60) * (*p == '-' ? -1 : 1);
  } else if (*p != 'Z' && *p != 'z') {
    return 0;
  }
  int64_t local = daysFromCivil(Y, M, D) * 86400 + h * 3600 + m * 60 + sec;
  return (time_t)(local - offSec);
}
