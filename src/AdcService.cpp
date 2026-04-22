#include <Arduino.h>
#include "AdcService.h"
#include <SD.h>

// --- Constants ---
static const uint8_t  SAMPLES_PER_TICK = 8;    // Multisample to reduce noise
static const uint8_t  EMA_SHIFT        = 3;    // alpha = 1/8
static const uint16_t ADC_VREF_MV      = 3300; // ESP32 default (kan kalibreres)

// tuner-område
static const uint8_t  TUNER_HYS_PCT    = 10;   // Hysteresis inside a bin window
static const uint16_t TUNER_STABLE_MS  = 200;  // Time before accepting a new bin

// Voltage divider (adjust to your PCB)
static const uint32_t VBAT_RTOP = 33000;
static const uint32_t VBAT_RBOT = 6800;

// volume deadband
static const uint8_t  VOL_DB_LOW_PCT  = 1;
static const uint8_t  VOL_DB_HIGH_PCT = 99;

// --- State ---
static bool     s_inited        = false;
static uint16_t s_ema_vol_12b   = 0;  // 0..4095
static uint16_t s_ema_vbat_12b  = 0;  // 0..4095
static uint16_t s_last_raw_vol  = 0;

static uint8_t  s_vol_pct       = 0;
static uint16_t s_vbat_mV       = 0;
static uint16_t s_ema_mode_12b  = 0;

// tuner-state
static volatile uint16_t s_tuner_min_raw = 0;
static volatile uint16_t s_tuner_max_raw = 4095;
static uint8_t  s_tuner_bin     = 0;  // 0..15
static uint32_t s_tuner_last_ms = 0;
static uint8_t  s_tuner_last_bin = 0;

// task handle
static TaskHandle_t s_adcTask = nullptr;

// --- Helper functions ---
static inline uint16_t map_u16(uint16_t x, uint16_t in_min, uint16_t in_max,
                               uint16_t out_min, uint16_t out_max) {
  if (x <= in_min) return out_min;
  if (x >= in_max) return out_max;
  return (uint16_t)((uint32_t)(x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min);
}

// Read a single channel with multisampling
static uint16_t readAdcAveraged(uint8_t pin) {
  uint32_t acc = 0;
  for (uint8_t i = 0; i < SAMPLES_PER_TICK; i++) {
    acc += (analogRead(pin) & 0x0FFF);
  }
  return (uint16_t)(acc / SAMPLES_PER_TICK);
}

static void updateTunerConfig(const char* key, uint16_t value) {
  String outContent = "";
  bool found = false;
  const char* path = "/system/sound/max_volumes.txt";
  
  File f = SD.open(path, FILE_READ);
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      String trimmed = line;
      trimmed.trim();
      if (trimmed.length() == 0) continue;
      
      if (trimmed.startsWith(key)) {
        outContent += String(key) + " " + String(value) + "\n";
        found = true;
      } else {
        outContent += trimmed + "\n";
      }
    }
    f.close();
  }
  
  if (!found) {
    outContent += String(key) + " " + String(value) + "\n";
  }

  if (!SD.exists("/system")) SD.mkdir("/system");
  if (!SD.exists("/system/sound")) SD.mkdir("/system/sound");

  f = SD.open(path, FILE_WRITE);
  if (f) {
    f.print(outContent);
    f.close();
  } else {
    Serial.println("[ADC] Failed to write calibration to SD");
  }
}

void adcInit() {
  if (s_inited) return;

  pinMode(PIN_VOL_POT,   INPUT);
  pinMode(PIN_TUNER_POT, INPUT);
  pinMode(PIN_VBAT_ADC,  INPUT);
  pinMode(PIN_MODE_SEL_ADC, INPUT);

  s_tuner_min_raw = 0;
  s_tuner_max_raw = 4095;

  File f = SD.open("/system/sound/max_volumes.txt", FILE_READ);
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.startsWith("TUNER_MIN_RAW")) {
        String valStr = line.substring(line.indexOf(' '));
        valStr.trim();
        s_tuner_min_raw = (uint16_t)valStr.toInt();
      } else if (line.startsWith("TUNER_MAX_RAW")) {
        String valStr = line.substring(line.indexOf(' '));
        valStr.trim();
        s_tuner_max_raw = (uint16_t)valStr.toInt();
      }
    }
    f.close();
  }
  Serial.printf("[ADC] Tuner bounds loaded: MIN=%u, MAX=%u\n", s_tuner_min_raw, s_tuner_max_raw);

  uint16_t v0 = analogRead(PIN_VOL_POT)  & 0x0FFF;
  uint16_t b0 = analogRead(PIN_VBAT_ADC) & 0x0FFF;
  uint16_t m0 = analogRead(PIN_MODE_SEL_ADC) & 0x0FFF;
  s_ema_vol_12b  = v0;
  s_ema_vbat_12b = b0;
  s_ema_mode_12b = m0;
  s_last_raw_vol = v0;

  s_vol_pct  = 0;
  s_vbat_mV  = 0;
  s_tuner_bin = 0;
  s_tuner_last_bin = 0;
  s_tuner_last_ms  = millis();

  s_inited = true;

  // Run in dedicated task to prevent blocking by Net/WiFi
  if (!s_adcTask) {
    xTaskCreatePinnedToCore(
      [](void*) {
        const TickType_t d = pdMS_TO_TICKS(10); // 100 Hz
        for (;;) {
          adcTick();
          vTaskDelay(d);
        }
      },
      "adcTask", 2048, nullptr, 4, &s_adcTask, 1
    );
  }
}

void adcTick() {
  if (!s_inited) return;

  uint32_t now = millis();

  // --- Volume ---
  uint16_t vol_raw = readAdcAveraged(PIN_VOL_POT);
  s_last_raw_vol = vol_raw;
  // EMA
  s_ema_vol_12b = s_ema_vol_12b - (s_ema_vol_12b >> EMA_SHIFT) + (vol_raw >> EMA_SHIFT);
  // 0..100 %
  uint8_t vol_pct = (uint8_t)((uint32_t)s_ema_vol_12b * 100 / 4095);
  // deadband
  if (vol_pct <= VOL_DB_LOW_PCT) vol_pct = 0;
  if (vol_pct >= VOL_DB_HIGH_PCT) vol_pct = 100;
  s_vol_pct = vol_pct;

  // --- Battery ---
  uint16_t vbat_raw = readAdcAveraged(PIN_VBAT_ADC);
  s_ema_vbat_12b = s_ema_vbat_12b - (s_ema_vbat_12b >> EMA_SHIFT) + (vbat_raw >> EMA_SHIFT);

  // Convert to mV
  // adc_mv = (ema / 4095) * Vref
  uint32_t adc_mv = (uint32_t)s_ema_vbat_12b * ADC_VREF_MV / 4095;
  // Voltage divider factor
  uint32_t vbat_mv = adc_mv * (VBAT_RTOP + VBAT_RBOT) / VBAT_RBOT;
  s_vbat_mV = (uint16_t)vbat_mv;

  // --- Tuner ---
uint16_t tuner_raw = readAdcAveraged(PIN_TUNER_POT);

uint8_t new_bin = 0;  // default: out of range

uint16_t t_start = s_tuner_min_raw;
uint16_t t_end   = s_tuner_max_raw;
bool reversed = t_start > t_end;
uint16_t t_min = reversed ? t_end : t_start;
uint16_t t_max = reversed ? t_start : t_end;

if (t_max - t_min < 16) t_max = t_min + 16;

if (tuner_raw >= t_min && tuner_raw <= t_max) {
  uint32_t span = t_max - t_min;
  uint32_t rel  = tuner_raw - t_min;
  uint8_t bin = (uint8_t)(rel * 16 / span);  // 0..15
  if (bin > 15) bin = 15;
  new_bin = reversed ? (16 - bin) : (bin + 1);
} else if (tuner_raw < t_min) {
  new_bin = reversed ? 16 : 1;
} else if (tuner_raw > t_max) {
  new_bin = reversed ? 1 : 16;
}

// Hysteresis/stability as before
if (new_bin != s_tuner_last_bin) {
  s_tuner_last_bin = new_bin;
  s_tuner_last_ms  = now;
} else {
  if ((now - s_tuner_last_ms) >= TUNER_STABLE_MS) {
    s_tuner_bin = new_bin;
  }
}

  // --- Mode selector ADC ---
  uint16_t mode_raw = readAdcAveraged(PIN_MODE_SEL_ADC);
  s_ema_mode_12b = s_ema_mode_12b - (s_ema_mode_12b >> EMA_SHIFT) + (mode_raw >> EMA_SHIFT);
}

// --- Getters ---
uint8_t adcVolumePct() {
  return s_vol_pct;
}

uint16_t adcRawVolume() {
  return s_last_raw_vol;
}

uint16_t adcBattery_mV() {
  return s_vbat_mV;
}

int8_t adcTunerBin16() {
  return (int8_t)s_tuner_bin;
}

uint16_t adcModeSelRaw12b() {
  return s_ema_mode_12b;
}

void adcSetTunerMin() {
  s_tuner_min_raw = readAdcAveraged(PIN_TUNER_POT);
  updateTunerConfig("TUNER_MIN_RAW", s_tuner_min_raw);
  Serial.printf("[ADC] Tuner MIN set to %u\n", s_tuner_min_raw);
}

void adcSetTunerMax() {
  s_tuner_max_raw = readAdcAveraged(PIN_TUNER_POT);
  updateTunerConfig("TUNER_MAX_RAW", s_tuner_max_raw);
  Serial.printf("[ADC] Tuner MAX set to %u\n", s_tuner_max_raw);
}
