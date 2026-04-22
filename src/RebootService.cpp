#include "RebootService.h"

#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>
#include "esp_system.h"

// IMPORTANT: We need to know all mode-stop functions here
#include "mode_bt.h"
#include "mode_mp3.h"
#include "mode_net.h"
#include "mode_idle.h"

#include "AudioService.h"   // for audioForceShutdown()

extern void audioMute(bool on); // Ensure access to the mute function

// Gracefully stop active mode
static void stopMode(AppMode m) {
  switch (m) {
    case MODE_BT:
      modeBtStop();
      break;
    case MODE_MP3:
      modeMp3Stop();
      break;
    case MODE_NET:
      modeNetStop();
      break;
    case MODE_IDLE:
    default:
      modeIdleStop();
      break;
  }
}

void rebootFromMode(AppMode currentMode)
{
  Serial.println("[SYS] controlled reboot requested");

  // 1) Stop the current mode
  stopMode(currentMode);

  // 2) Ensure silence + close I2S regardless of owner
  audioForceShutdown();

  // 3) Completely disable WiFi (safe even if Net mode already did)
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);

  // 4) Unmount SD card if present
  if (SD.cardType() != CARD_NONE) {
    SD.end();
    Serial.println("[SYS] SD unmounted");
  }

  Serial.println("[SYS] restarting ESP32 ...");
  Serial.flush();
  delay(150);
  esp_restart();
}

void safeShutdown(AppMode currentMode)
{
  Serial.println("\n[SYS] CRITICAL: Low voltage (<12V) detected!");
  Serial.println("[SYS] Initiating safe shutdown to protect battery...");

  // 1) Mute the amplifier immediately to reduce power draw.
  // This extends the capacitor discharge time, giving the SD card more time to write.
  audioMute(true);

  // 2) Stop the current mode (ensures MP3 state and buffers are flushed to SD)
  stopMode(currentMode);

  // 3) Stop audio processes and I2S
  audioForceShutdown();

  // 4) Turn off WiFi and unmount SD card
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);

  if (SD.cardType() != CARD_NONE) {
    SD.end();
    Serial.println("[SYS] SD unmounted safely");
  }

  Serial.println("[SYS] Entering deep sleep. Goodnight!");
  Serial.flush();
  delay(100);

  // 5) Halt all execution to prevent further battery drain and operations
  while (true) {
    delay(1000);
  }
}
