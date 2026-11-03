// Speaker alert via the on-board ES8311 codec.
// Codec init follows Waveshare's examples/arduino/07_ES8311; the chime is synthesised in code.
#include "alert_audio.h"
#include <Arduino.h>
#include <math.h>
#include "ESP_I2S.h"
#include "esp_check.h"
#include "es8311.h"
#include "pin_config.h"

static const int SAMPLE_RATE = 16000;
static const int VOLUME = 85;          // codec volume 0-100
static const float ALERT_LEVEL = 0.275f;  // chime amplitude 0..1 (was 0.55; halved on request)
static const int PIN_MCLK = 42, PIN_BCLK = 9, PIN_WS = 45, PIN_DOUT = 8, PIN_DIN = 10;
// PA (GPIO46, speaker amp enable) comes from pin_config.h

static I2SClass i2s;
static TaskHandle_t audioTask;
static bool audioReady = false;

static bool codecInit() {
  es8311_handle_t es = es8311_create(0 /* I2C port, same bus as Wire */, ES8311_ADDRESS_0);
  if (!es) return false;
  const es8311_clock_config_t clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = SAMPLE_RATE * 256,
    .sample_frequency = SAMPLE_RATE,
  };
  if (es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
  if (es8311_sample_frequency_config(es, clk.mclk_frequency, clk.sample_frequency) != ESP_OK) return false;
  if (es8311_voice_volume_set(es, VOLUME, nullptr) != ESP_OK) return false;
  return true;
}

// Write `ms` of a sine tone (freq 0 = silence) with 5 ms fade in/out to avoid clicks.
static void writeTone(float freq, int ms) {
  static int16_t buf[256 * 2];                     // stereo frames
  const int total = SAMPLE_RATE * ms / 1000;
  const int fade = SAMPLE_RATE * 5 / 1000;
  const float amp = ALERT_LEVEL * 32767.0f;
  float phase = 0, step = 2.0f * (float)M_PI * freq / SAMPLE_RATE;
  for (int done = 0; done < total;) {
    int n = min(256, total - done);
    for (int i = 0; i < n; i++) {
      int idx = done + i;
      float env = 1.0f;
      if (idx < fade) env = (float)idx / fade;
      else if (idx > total - fade) env = (float)(total - idx) / fade;
      int16_t s = freq > 0 ? (int16_t)(sinf(phase) * amp * env) : 0;
      phase += step;
      if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
      buf[2 * i] = buf[2 * i + 1] = s;
    }
    i2s.write((const uint8_t *)buf, n * 2 * sizeof(int16_t));
    done += n;
  }
}

static void audioTaskFn(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    digitalWrite(PA, HIGH);
    writeTone(0, 30);                              // let the amp settle
    for (int rep = 0; rep < 2; rep++) {
      writeTone(880.0f, 150);  writeTone(0, 60);   // A5
      writeTone(1175.0f, 150); writeTone(0, 60);   // D6
      writeTone(1568.0f, 220); writeTone(0, 400);  // G6
    }
    writeTone(0, 50);
    digitalWrite(PA, LOW);
  }
}

bool audioBegin() {
  pinMode(PA, OUTPUT);
  digitalWrite(PA, LOW);                           // amp off when idle (no hiss)

  i2s.setPins(PIN_BCLK, PIN_WS, PIN_DOUT, PIN_DIN, PIN_MCLK);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("[audio] I2S init failed");
    return false;
  }
  if (!codecInit()) {
    Serial.println("[audio] ES8311 init failed");
    return false;
  }
  xTaskCreatePinnedToCore(audioTaskFn, "audio", 4096, nullptr, 2, &audioTask, 0);
  audioReady = true;
  Serial.println("[audio] ready");
  return true;
}

void audioPlayAlert() {
  if (audioReady) xTaskNotifyGive(audioTask);
}
