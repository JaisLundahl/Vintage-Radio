#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include "output_service.h"

#include "pins.h"
#include "AdcService.h"
#include "InputService.h"
#include "mode_bt.h"
#include "mode_mp3.h"    
#include "mode_net.h"
#include "mode_idle.h"
#include "AudioService.h"
#include "RebootService.h"

extern void safeShutdown(AppMode currentMode); // Ensure signature is available

static AppMode g_mode = MODE_IDLE;
static AppMode g_pendingMode = MODE_IDLE;
static unsigned long g_modeChangeTs = 0;
static bool g_isCalibratingTuner = false;
static const unsigned long MODE_CHANGE_DELAY_MS = 100;  // debounce for drejevælger
static const uint32_t REBOOT_GRACE_MS = 2000;  // 2 sek efter boot


/*
 * =========================================================================================
 * VINTAGE RADIO - SD CARD FILE STRUCTURE
 * =========================================================================================
 * For a virgin SD card, the following directory structure and files are expected:
 * 
 * / (Root)
 *  ├── /folder1             (REQUIRED for MP3) Default folders for the 16 tuner bins. 
 *  ├── ...                  Files inside must be named exactly 8 digits: "00000001.mp3".
 *  └── /folder16            
 * 
 * /system                   System configuration and asset directory
 *  ├── /sound
 *  │    ├── wait.mp3        (REQUIRED) Played while tuning between stations or loading.
 *  │    └── max_volumes.txt (OPTIONAL) Defines limits: MASTER_REG_MAX (0x30 for 0dB) 
 *  │                        and WAIT_VOL_MAX (e.g. 15 for 15% of current master volume).
 *  │
 *  ├── /netradio
 *  │    ├── WIFI.txt        (REQUIRED for NET) Format: SSID|PASSWORD per line.
 *  │    └── stations.txt    (REQUIRED for NET) Format: Name|URL|type per line (max 16).
 *  │
 *  └── /mp3
 *       ├── mp3_folders.txt (OPTIONAL) Custom paths for the 16 tuner bins instead of /folderX.
 *       └── mp3_state.txt   (GENERATED) Automatically created by the system. Saves track 
 *                           history, last played, and max indices to persist between reboots.
 * =========================================================================================
 */

// Only used during boot
static void startMode(AppMode m) {
  g_mode = m;
  switch (g_mode) {
    case MODE_BT:   modeBtStart();   break;
    case MODE_MP3:  modeMp3Start();  break;
    case MODE_NET:  modeNetStart();  break;
    case MODE_IDLE: modeIdleStart(); break;
  }
  Serial.printf("Booted into mode %d\n", (int)g_mode);
  outputService().setState(SystemState::READY);
}

// At boot: wait until selector has maintained the same value for stableMs
static AppMode waitForStableSelector(uint32_t stableMs)
{
  uint32_t start = millis();
  AppMode  cur   = inputsCurrentMode();
  for (;;) {
    inputsTick();
    AppMode m = inputsCurrentMode();
    if (m != cur) {
      cur = m;
      start = millis();
    }
    if (millis() - start >= stableMs) {
      return cur;
    }
    delay(5);
  }
}
void setup() {
  outputService().begin();
  outputService().setState(SystemState::INIT);
  Serial.begin(115200);
  delay(50);
  Serial.println("\nBooting...");

  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI);
  if (!SD.begin(PIN_SD_CS, SPI, 10000000)){
    Serial.println("[SD] Init failed!");
    outputService().setState(SystemState::ERROR_STATE);
  } else {
    Serial.println("[SD] Mounted successfully");
  }

  inputsInit();
  adcInit();
  audioInit();

  // NEW: we only verify a stable selector here
  AppMode bootMode = waitForStableSelector(2000);
  // Set the amplifier to the physical volume knob's level before starting any modes
  audioSetVolume(adcVolumePct());
  
  startMode(bootMode);
  g_pendingMode  = bootMode;
  g_modeChangeTs = millis();
}

void loop() {
  inputsTick();

  InputEvent ev;
  while (inputsGetEvent(ev)) {
    switch (ev.type) {
      case EV_ROT_CW: //select next number
        if (g_mode == MODE_BT)      btNext();
        else if (g_mode == MODE_MP3) modeMp3Next();
        else if (g_mode == MODE_NET) modeNetReload();
        break;

        case EV_ROT_CCW: //select previous number
        if (g_mode == MODE_BT)      btPrev();
        else if (g_mode == MODE_MP3) modeMp3Prev();
        else if (g_mode == MODE_NET) modeNetReload();
        break;
      
        case EV_SHORT_PRESS:// toggle play/pause
        if (g_mode == MODE_BT)       btPlayPauseToggle();
        else if (g_mode == MODE_MP3) modeMp3PlayPauseToggle();
        else if (g_mode == MODE_NET) modeNetPlayPauseToggle();
        break;

        case EV_LONG_PRESS:// enter pairing mode /WPS:
        if (g_mode == MODE_BT)       btEnterPairMode();
        else if (g_mode == MODE_NET) WPS();
        break;

      case EV_VERY_LONG_PRESS:
        if (adcVolumePct() == 0) {
          Serial.println("[CAL] Calibration started - setting MIN");
          adcSetTunerMin();
          outputService().pulseRed4s();
          g_isCalibratingTuner = true;
        }
        break;

      case EV_VERY_LONG_RELEASE:
        if (g_isCalibratingTuner) {
          Serial.println("[CAL] Calibration ended - setting MAX");
          adcSetTunerMax();
          outputService().pulseGreen4s();
          g_isCalibratingTuner = false;
        }
        break;

      case EV_MODE_CHANGED:
        Serial.printf("[INPUT] EV_MODE_CHANGED -> %d (t=%lu)\n",
                      (int)ev.mode, (unsigned long)millis());
        g_pendingMode  = ev.mode;
        g_modeChangeTs = millis();
        break;
    }
  }

  // Rotary selector debounce - once the delay passes: REBOOT
if (g_pendingMode != g_mode) {
  if (millis() > REBOOT_GRACE_MS &&
      millis() - g_modeChangeTs > MODE_CHANGE_DELAY_MS) {
    rebootFromMode(g_mode);
  }
}
outputService().loop();

  // Low voltage protection check (under 12V = 12000mV).
  // Wait 5 seconds after boot to let the ADC EMA stabilize.
  if (millis() > 5000) {
    uint16_t vbat = adcBattery_mV();
    if (vbat > 1000 && vbat < 12000) {
      safeShutdown(g_mode);
    }
  }

  // Service the active mode's background tasks
  switch (g_mode) {
    case MODE_BT:   modeBtLoop();   break;
    case MODE_IDLE: modeIdleLoop(); break;
    default: break; // MP3 and NET modes run in their own FreeRTOS tasks
  }

  // Poll the volume pot at a modest rate; pushing I2C amp updates every loop adds jitter.
  static unsigned long tLastVolume = 0;
  if (millis() - tLastVolume >= 50) {
    tLastVolume = millis();
    uint8_t vol = adcVolumePct();
    audioSetVolume(vol);
    if (g_mode == MODE_BT) {
      btSetVolumePercent(vol);
    }
  }

  // Keep serial traffic low while BT audio is active; UART printing can add jitter.
  static unsigned long tLast = 0;
  if (g_mode != MODE_BT && millis() - tLast > 500) {
    tLast = millis();
    uint8_t  vol = adcVolumePct();
    int8_t   bin = adcTunerBin16();
    uint16_t vb  = adcBattery_mV();
    const char* modeName =
      (g_mode == MODE_BT)  ? "BT"  :
      (g_mode == MODE_MP3) ? "MP3" :
      (g_mode == MODE_NET) ? "NET" : "IDLE";

    Serial.printf("VOL=%3u%%  TUNER=%02d  VBAT=%.2fV  MODE=%s\n",
                  vol, bin, vb/1000.0f, modeName);
  }
}
