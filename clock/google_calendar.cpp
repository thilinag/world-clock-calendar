// Google Calendar "next event" fetcher.
//
// Auth: OAuth 2.0 refresh token (obtained once on a PC with tools/get_google_refresh_token.ps1,
// stored in secrets.h). The task trades it for a short-lived access token and calls
// Calendar API v3 events.list on the primary calendar every few minutes.
#include "google_calendar.h"
#include "time_utils.h"
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h"

#ifndef GOOGLE_CLIENT_ID
#define GOOGLE_CLIENT_ID ""
#endif
#ifndef GOOGLE_CLIENT_SECRET
#define GOOGLE_CLIENT_SECRET ""
#endif
#ifndef GOOGLE_REFRESH_TOKEN
#define GOOGLE_REFRESH_TOKEN ""
#endif

static const uint32_t POLL_MS        = 3UL * 60UL * 1000UL;   // normal refresh interval
static const uint32_t RETRY_MS       = 30UL * 1000UL;         // after a network/API error
static const uint32_t AUTH_RETRY_MS  = 10UL * 60UL * 1000UL;  // after the refresh token was rejected
static const Zone    *homeZone = nullptr;                     // "today" is measured in this zone

static SemaphoreHandle_t stateMutex;
static CalendarState state;
static TaskHandle_t calTaskHandle;
static String accessToken;
static time_t accessTokenExpiry = 0;

// ------------------------------------------------------------------ helpers
static String urlEncode(const char *s) {
  static const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(strlen(s) * 3);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
      out += (char)*p;
    } else {
      out += '%';
      out += hex[*p >> 4];
      out += hex[*p & 15];
    }
  }
  return out;
}

static void isoUtc(time_t t, char *buf, size_t n) {
  struct tm u;
  gmtime_r(&t, &u);
  strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &u);
}

// UTC instant of the coming local midnight in the home zone.
static time_t endOfToday(time_t now) {
  struct tm lt;
  int off = localTimeIn(*homeZone, now, &lt);
  int64_t nextMidnightLocal = (((int64_t)now + off) / 86400 + 1) * 86400;
  time_t t = (time_t)(nextMidnightLocal - off);
  off = localTimeIn(*homeZone, t, &lt);           // re-check offset at midnight (DST change days)
  return (time_t)(nextMidnightLocal - off);
}

static void setStatus(CalStatus s, int httpCode = 0) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (state.status != s || state.lastHttp != httpCode) {
    state.status = s;
    state.lastHttp = httpCode;
    state.version++;
  }
  xSemaphoreGive(stateMutex);
}

static void publishEvents(const CalEvent *events, int count) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  state.count = count;
  for (int i = 0; i < count; i++) state.events[i] = events[i];
  state.status = CAL_OK;
  state.lastHttp = 0;
  state.updatedAt = time(nullptr);
  state.version++;
  xSemaphoreGive(stateMutex);
}

static void copyStr(char *dst, size_t n, const char *src) {
  strncpy(dst, src ? src : "", n - 1);
  dst[n - 1] = '\0';
}

// ------------------------------------------------------------------ OAuth
// Returns HTTP status (200 = ok). *authError is set when Google rejects the refresh token/client.
static int refreshAccessToken(bool *authError) {
  *authError = false;
  NetworkClientSecure client;
  client.useBuiltinCACertBundle();
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, "https://oauth2.googleapis.com/token")) return -1;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "grant_type=refresh_token&client_id=" + urlEncode(GOOGLE_CLIENT_ID) +
                "&client_secret=" + urlEncode(GOOGLE_CLIENT_SECRET) +
                "&refresh_token=" + urlEncode(GOOGLE_REFRESH_TOKEN);
  int code = http.POST(body);
  String resp = code > 0 ? http.getString() : String();
  http.end();

  JsonDocument doc;
  deserializeJson(doc, resp);
  if (code == 200 && doc["access_token"].is<const char *>()) {
    accessToken = doc["access_token"].as<const char *>();
    accessTokenExpiry = time(nullptr) + (doc["expires_in"] | 3600) - 60;
    Serial.println("[cal] access token refreshed");
    return 200;
  }

  const char *err = doc["error"] | "";
  Serial.printf("[cal] token refresh failed: HTTP %d %s\n", code, err);
  if (!strcmp(err, "invalid_grant") || !strcmp(err, "invalid_client") || !strcmp(err, "unauthorized_client"))
    *authError = true;
  return code;
}

// ------------------------------------------------------------------ Calendar API
// Returns HTTP status (200 = ok).
static int fetchEvents() {
  time_t now = time(nullptr);
  char tMin[24], tMax[24];
  isoUtc(now, tMin, sizeof(tMin));
  isoUtc(endOfToday(now), tMax, sizeof(tMax));      // only events starting today

  String url = "https://www.googleapis.com/calendar/v3/calendars/primary/events"
               "?singleEvents=true&orderBy=startTime&maxResults=20"
               "&timeMin=" + urlEncode(tMin) + "&timeMax=" + urlEncode(tMax) +
               "&fields=" + urlEncode("items(id,summary,start,end,status,eventType,attendees(self,responseStatus))");

  NetworkClientSecure client;
  client.useBuiltinCACertBundle();
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) return -1;
  http.addHeader("Authorization", "Bearer " + accessToken);
  int code = http.GET();
  if (code != 200) {
    String resp = code > 0 ? http.getString() : String();
    http.end();
    Serial.printf("[cal] events.list failed: HTTP %d %.200s\n", code, resp.c_str());
    return code;
  }

  JsonDocument filter;
  JsonObject f = filter["items"].add<JsonObject>();
  f["id"] = true;
  f["summary"] = true;
  f["status"] = true;
  f["eventType"] = true;
  f["start"]["dateTime"] = true;
  f["end"]["dateTime"] = true;
  JsonObject fa = f["attendees"].add<JsonObject>();
  fa["self"] = true;
  fa["responseStatus"] = true;

  // Read the whole body first (handles chunked encoding; parsing straight from the TLS
  // stream timed out mid-response -> "IncompleteInput"). The `fields` filter keeps it small.
  String body = http.getString();
  http.end();
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[cal] JSON error: %s (%u bytes)\n", err.c_str(), body.length());
    return -2;
  }

  CalEvent found[CAL_MAX_EVENTS];
  int n = 0;
  for (JsonObject it : doc["items"].as<JsonArray>()) {
    if (n >= CAL_MAX_EVENTS) break;
    if (!strcmp(it["status"] | "", "cancelled")) continue;
    const char *type = it["eventType"] | "default";
    if (!strcmp(type, "workingLocation") || !strcmp(type, "outOfOffice")) continue;
    const char *startStr = it["start"]["dateTime"];
    if (!startStr) continue;                  // all-day event
    bool declined = false;
    for (JsonObject a : it["attendees"].as<JsonArray>()) {
      if ((a["self"] | false) && !strcmp(a["responseStatus"] | "", "declined")) declined = true;
    }
    if (declined) continue;
    time_t start = parseRfc3339(startStr);
    if (start <= now) continue;               // already started
    CalEvent &e = found[n++];
    copyStr(e.id, sizeof(e.id), it["id"] | "");
    copyStr(e.title, sizeof(e.title), it["summary"] | "(No title)");
    e.start = start;
    e.end = parseRfc3339(it["end"]["dateTime"] | "");
  }

  publishEvents(found, n);
  if (n > 0) Serial.printf("[cal] %d upcoming; next: \"%s\" in %ld s\n", n, found[0].title, (long)(found[0].start - now));
  else       Serial.println("[cal] no upcoming events");
  return 200;
}

// ------------------------------------------------------------------ task
static void injectTestEvents() {
  time_t now = time(nullptr);
  CalEvent ev[2] = {};
  copyStr(ev[0].id, sizeof(ev[0].id), "test-1");
  copyStr(ev[0].title, sizeof(ev[0].title), "Test meeting");
  ev[0].start = now + CAL_TEST_EVENT_IN_SEC;
  ev[0].end = ev[0].start + 30 * 60;
  copyStr(ev[1].id, sizeof(ev[1].id), "test-2");
  copyStr(ev[1].title, sizeof(ev[1].title), "Follow-up sync");
  ev[1].start = ev[0].start + 45 * 60;
  ev[1].end = ev[1].start + 15 * 60;
  publishEvents(ev, 2);
  Serial.printf("[cal] TEST MODE: fake event in %d s\n", CAL_TEST_EVENT_IN_SEC);
}

static void calTask(void *) {
  uint32_t waitMs = 1000;
  bool testInjected = false;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs));

    if (CAL_TEST_EVENT_IN_SEC > 0) {
      if (!testInjected && timeValid()) { injectTestEvents(); testInjected = true; }
      waitMs = POLL_MS;
      continue;
    }

    if (WiFi.status() != WL_CONNECTED || !timeValid()) {
      waitMs = 3000;
      continue;
    }

    if (accessToken.isEmpty() || time(nullptr) >= accessTokenExpiry) {
      bool authError;
      int code = refreshAccessToken(&authError);
      if (code != 200) {
        setStatus(authError ? CAL_AUTH_ERROR : CAL_NET_ERROR, code);
        waitMs = authError ? AUTH_RETRY_MS : RETRY_MS;
        continue;
      }
    }

    int code = fetchEvents();
    if (code == 200) {
      waitMs = POLL_MS;
    } else if (code == 401) {                 // access token expired early -> refresh and retry
      accessToken = "";
      waitMs = 1000;
    } else {
      setStatus(CAL_NET_ERROR, code);
      waitMs = RETRY_MS;
    }
  }
}

// ------------------------------------------------------------------ API
void calendarBegin(const Zone *home) {
  homeZone = home;
  stateMutex = xSemaphoreCreateMutex();
  memset(&state, 0, sizeof(state));
  bool configured = strlen(GOOGLE_CLIENT_ID) && strlen(GOOGLE_REFRESH_TOKEN);
  state.status = (configured || CAL_TEST_EVENT_IN_SEC > 0) ? CAL_WAITING : CAL_NOT_CONFIGURED;
  state.version = 1;
  if (state.status == CAL_NOT_CONFIGURED) {
    Serial.println("[cal] not configured: add GOOGLE_* values to secrets.h");
    return;
  }
  // Core 0 (WiFi core) so TLS handshakes never stall the UI loop on core 1.
  xTaskCreatePinnedToCore(calTask, "calendar", 16384, nullptr, 1, &calTaskHandle, 0);
}

void calendarRefreshNow() {
  if (calTaskHandle) xTaskNotifyGive(calTaskHandle);
}

void calendarGetState(CalendarState *out) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  *out = state;
  xSemaphoreGive(stateMutex);
}
