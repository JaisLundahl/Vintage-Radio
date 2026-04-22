// mode_bt.cpp - A2DP sink for ESP32 with safe stop for reboot
#include "mode_bt.h"
#include "pins.h"
#include "driver/i2s.h"
#include <Arduino.h>
#include <WiFi.h>
#include <BluetoothA2DPSink.h>
#include <Preferences.h>

#include "AudioService.h"

static BluetoothA2DPSink a2dp;
static Preferences        prefs;

static bool     s_running     = false;
static bool     s_connected   = false;
static bool     s_stopping    = false;
static uint32_t s_startedMs   = 0;
static uint8_t  s_lastBtVol   = 255;

static const char* NVS_NS   = "bt";
static const char* NVS_LAST = "lastaddr";

#ifndef BT_NAME
#define BT_NAME "Filips BoomBox"
#endif

// -----------------------------------------------------------------------------
// I2S pins for BT
// -----------------------------------------------------------------------------
static i2s_pin_config_t bt_i2s_pins() {
  i2s_pin_config_t pins = {};
  pins.bck_io_num   = PIN_I2S_BCK;
  pins.ws_io_num    = PIN_I2S_LRCK;
  pins.data_out_num = PIN_I2S_DATA;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  return pins;
}
// I2S config for BT
static i2s_config_t bt_i2s_config() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    // This is the largest stable profile we had before the boot-loop regression.
    .dma_buf_count = 16,
    .dma_buf_len   = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  return cfg;
}
// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static void set_discoverable(bool on) {
  a2dp.set_connectable(true);
  a2dp.set_discoverability(on ? ESP_BT_GENERAL_DISCOVERABLE
                              : ESP_BT_NON_DISCOVERABLE);
}

static void save_peer(const esp_bd_addr_t* peer){
  if (!peer) return;
  char s[18];
  sprintf(s, "%02x:%02x:%02x:%02x:%02x:%02x",
          (*peer)[0], (*peer)[1], (*peer)[2],
          (*peer)[3], (*peer)[4], (*peer)[5]);
  prefs.begin(NVS_NS, false);
  prefs.putString(NVS_LAST, s);
  prefs.end();
}

// Callback from A2DP library
static void on_connection_state_changed(esp_a2d_connection_state_t state, void*) {
  switch (state) {
    case ESP_A2D_CONNECTION_STATE_CONNECTED:
      s_connected = true;
      save_peer(a2dp.get_current_peer_address());
      set_discoverable(false);
      break;

    case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
      s_connected = false;
      // Important: only auto-reconnect if we aren't currently stopping
      if (!s_stopping) {
        a2dp.reconnect();
        set_discoverable(true);
      }
      break;

    default:
      break;
  }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
void modeBtStart() {
  if (s_running) return;

  WiFi.mode(WIFI_OFF);

  if (!audioStartBtMode()) {
    Serial.println("[BT] could not acquire I2S for BT");
    return;
  }

  a2dp.set_i2s_config(bt_i2s_config());   // ← NEW
  a2dp.set_pin_config(bt_i2s_pins());     // ← Your existing config
  a2dp.set_auto_reconnect(true);
  a2dp.set_on_connection_state_changed(on_connection_state_changed);
  a2dp.start(BT_NAME);
  audioEnsurePlaybackPath();
  // Mute is often active from MP3 mode - disable it
  audioMute(false);

  s_running   = true;
  s_stopping  = false;
  s_startedMs = millis();

  // Try last known device
  prefs.begin(NVS_NS, true);
  String last = prefs.getString(NVS_LAST, "");
  prefs.end();
  if (last.length() == 17) {
    // Known device found - attempt to reconnect
    a2dp.reconnect();
    set_discoverable(false);
  } else {
    // No previous device - enter discoverable mode
    set_discoverable(true);
  }

  Serial.println("[BT] start");
}

void btSetVolumePercent(uint8_t percent) {
  if (!s_running || !s_connected) return;
  if (percent > 100) percent = 100;
  if (s_lastBtVol == percent) return;

  // AVRCP absolute volume is 0..127.
  uint8_t avrcpVol = (uint8_t)(((uint16_t)percent * 127u + 50u) / 100u);
  a2dp.set_volume(avrcpVol);
  s_lastBtVol = percent;
}

void modeBtStop() {
  if (!s_running) return;

  Serial.println("[BT] stop requested");
  s_stopping = true;

  // Ensure we don't start reconnecting while stopping
  a2dp.set_auto_reconnect(false);

  // Close A2DP - depends on connection status
  if (s_connected) {
    // true = also close AVRCP connection
    a2dp.end(true);
  } else {
    a2dp.end(false);
  }

  // Short safety pause - A2DP cleanup can be slightly slow
  delay(50);

  // Release I2S back to the system
  audioReleaseBt();

  s_running   = false;
  s_connected = false;
  s_stopping  = false;
  s_lastBtVol = 255;

  Serial.println("[BT] stopped");
}

// We still have canStop() for legacy calls - but with reboot architecture
// it is fine to simply return true
bool modeBtCanStop() {
  // Previously we waited for "not connecting right now".
  // With reboot it is better to just return "yes"
  return true;
}

void modeBtLoop() {
  // Nothing here currently - BT library runs itself
}

void btPlayPauseToggle() {
  if (!s_running) return;
  auto st = a2dp.get_audio_state();
  if (st == ESP_A2D_AUDIO_STATE_STARTED) a2dp.pause();
  else                                    a2dp.play();
}

void btNext() {
  if (!s_running) return;
  a2dp.next();
}

void btPrev() {
  if (!s_running) return;
  a2dp.previous();
}

void btEnterPairMode() {
  if (!s_running) return;
  set_discoverable(true);
  Serial.println("[BT] pair mode");
}
