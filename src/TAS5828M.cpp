#include "TAS5828M.h"
#include <Arduino.h>
#include <Wire.h>
#include <SD.h>
#include "pins.h"

static bool    s_ampFound         = false;
static uint8_t s_ampAddr          = 0x60; // Default with pin 13 pulled to GND
static uint8_t s_appliedVolumePct = 0xFF;
static bool    s_appliedMute      = true;
static int32_t s_regMax           = 0x00; // Default +24dB limit

static const uint8_t TAS_REG_BOOK        = 0x7F;
static const uint8_t TAS_REG_PAGE        = 0x00;
static const uint8_t TAS_REG_RESET_CTRL  = 0x01;
static const uint8_t TAS_REG_DEVICE_CTRL = 0x03;
static const uint8_t TAS_REG_SAP_CTRL1   = 0x33;
static const uint8_t TAS_REG_SAP_CTRL2   = 0x34;
static const uint8_t TAS_REG_SAP_CTRL3   = 0x35;
static const uint8_t TAS_REG_VOLUME      = 0x4C;

static void ampWrite(uint8_t reg, uint8_t val) {
  if (!s_ampFound) return;
  Wire.beginTransmission(s_ampAddr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void TAS5828M::init() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);   // 100 kHz for TAS5828M

  File f = SD.open("/system/sound/max_volumes.txt", FILE_READ);
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.startsWith("MASTER_REG_MAX")) {
        String valStr = line.substring(line.indexOf(' '));
        valStr.trim();
        s_regMax = (int32_t)strtol(valStr.c_str(), NULL, 0);
        Serial.printf("[AMP] Loaded MASTER_REG_MAX: 0x%02X\n", (unsigned int)s_regMax);
      }
    }
    f.close();
  }

  Wire.beginTransmission(s_ampAddr);
  if (Wire.endTransmission() != 0) {
    Wire.beginTransmission(s_ampAddr);
  }

  if (Wire.endTransmission() == 0) {
    s_ampFound = true;
    Serial.println("[AMP] TAS5828M detected");

    // Perform a full factory reset of the amplifier parameters on startup
    ampWrite(TAS_REG_BOOK, 0x00);
    ampWrite(TAS_REG_PAGE, 0x00);
    ampWrite(TAS_REG_RESET_CTRL, 0x11); // 0x11 resets both the DSP and the registers
    delay(50); // IMPORTANT: Give the TAS5828M time to completely reboot

    ensurePlaybackPath();
    setVolumeAndMute(100, false, true); // Will be overridden shortly by AudioService
  } else {
    s_ampFound = false;
    Serial.println("[AMP] No TAS5828M detected – debug mode");
  }
}

bool TAS5828M::isDetected() {
  return s_ampFound;
}

void TAS5828M::setVolumeAndMute(uint8_t volPct, bool mute, bool force) {
  if (!s_ampFound) return;

  if (!force && s_appliedVolumePct == volPct && s_appliedMute == mute) {
    return;
  }

  const int32_t regMin = 0xC0;
  uint8_t regVal = (uint8_t)(regMin + (s_regMax - regMin) * (int32_t)volPct / 100);

  ampWrite(TAS_REG_BOOK, 0x00);
  ampWrite(TAS_REG_PAGE, 0x00);
  ampWrite(TAS_REG_VOLUME, regVal);
  ampWrite(TAS_REG_DEVICE_CTRL, (mute || volPct == 0) ? 0x0B : 0x03);

  s_appliedVolumePct = volPct;
  s_appliedMute = (mute || volPct == 0);
}

void TAS5828M::ensurePlaybackPath() {
  if (!s_ampFound) return;
  ampWrite(TAS_REG_BOOK, 0x00);
  ampWrite(TAS_REG_PAGE, 0x00);
  ampWrite(TAS_REG_SAP_CTRL1, 0x00);
  ampWrite(TAS_REG_SAP_CTRL2, 0x00);
  ampWrite(TAS_REG_SAP_CTRL3, 0x11);
  ampWrite(0x78, 0x80); // Clear any clock faults
  ampWrite(TAS_REG_DEVICE_CTRL, s_appliedMute ? 0x0B : 0x03);
}