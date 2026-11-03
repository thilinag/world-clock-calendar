// Google Calendar "next event" fetcher (OAuth refresh-token flow, runs on its own task).
#pragma once
#include <Arduino.h>
#include <time.h>

// Set to e.g. 90 to inject fake events (first one starts N seconds after boot)
// so the countdown/alert can be tested without Google. 0 = use Google Calendar.
#ifndef CAL_TEST_EVENT_IN_SEC
#define CAL_TEST_EVENT_IN_SEC 0
#endif

enum CalStatus {
  CAL_NOT_CONFIGURED,  // no client id / refresh token in secrets.h
  CAL_WAITING,         // waiting for WiFi / time / first fetch
  CAL_OK,
  CAL_AUTH_ERROR,      // refresh token rejected (expired / revoked)
  CAL_NET_ERROR,       // network or API error (last good data is kept)
};

struct CalEvent {
  char id[72];
  char title[96];
  time_t start;   // UTC
  time_t end;     // UTC
};

static const int CAL_MAX_EVENTS = 10;   // next event + up to 9 in the scrollable "Later" list

struct CalendarState {
  CalStatus status;
  int count;                       // upcoming events, sorted by start
  CalEvent events[CAL_MAX_EVENTS];
  time_t updatedAt;                // UTC of last successful fetch (0 = never)
  int lastHttp;                    // HTTP status of the last failure (0 = none)
  uint32_t version;                // bumps on every change
};

struct Zone;
void calendarBegin(const Zone *home);   // start the background task (call after WiFi.begin); "today" = home zone
void calendarRefreshNow();         // ask the task to fetch immediately
void calendarGetState(CalendarState *out);   // thread-safe copy
