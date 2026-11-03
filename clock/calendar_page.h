// "Next event" page (tile 2) and the full-screen red countdown overlay.
#pragma once
#include <lvgl.h>
#include "time_utils.h"
#include "google_calendar.h"

void calendarPageCreate(lv_obj_t *tile, const Zone *homeZone);
void calendarPageUpdate(time_t now, const CalendarState &st);
void calendarPageScrollTop();

void countdownShow(const char *title);
void countdownSet(long seconds);
void countdownHide();

// Events whose start is still in the future (the cached list can go stale between fetches).
static inline int calendarUpcoming(const CalendarState &st, time_t now, const CalEvent **out, int max) {
  int n = 0;
  for (int i = 0; i < st.count && n < max; i++)
    if (st.events[i].start > now) out[n++] = &st.events[i];
  return n;
}
