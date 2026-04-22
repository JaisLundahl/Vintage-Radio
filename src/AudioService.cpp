#include "AudioService.h"

#include <AudioTools.h>
#include <AudioTools/AudioCodecs/CodecMP3Helix.h>
#include <Wire.h>
#include <SD.h>
#include "pins.h"
#include "driver/i2s.h"
#include "TAS5828M.h"

using namespace audio_tools;

// -----------------------------------------------------------------------------
// Local objects
// -----------------------------------------------------------------------------
static I2SStream          s_i2s;
static MP3DecoderHelix    s_decoder;

// The Helix decoder fires notifyAudioChange({0,0,0}) when begin() resets it.
// I2SStream::setAudioInfo() has an unconditional assert(sample_rate != 0), so
// we intercept the notification and silently discard any zero-valued AudioInfo.
class AudioInfoGuard : public AudioOutput {
public:
  explicit AudioInfoGuard(I2SStream& sink) : p_sink(&sink) {}

  void setAudioInfo(AudioInfo info) override {
    if (info.sample_rate && info.channels && info.bits_per_sample) {
      AudioOutput::setAudioInfo(info);  // store & notify own listeners
      p_sink->setAudioInfo(info);       // forward to I2S only when valid
      
      // Changing I2S parameters halts the clocks momentarily, causing the TAS5828M 
      // to throw a clock fault and mute itself. We must clear the fault and wake 
      // the amplifier back up now that the new clocks are stable!
      audioEnsurePlaybackPath();
    }
    // silently drop {0,0,0} that the decoder fires on every begin()
  }

  size_t write(const uint8_t* buf, size_t len) override {
    return p_sink->write(buf, len);
  }

  int availableForWrite() override { return p_sink->availableForWrite(); }

private:
  I2SStream* p_sink;
};

static AudioInfoGuard     s_guard(s_i2s);               // sits between s_out and s_i2s
static EncodedAudioStream s_out(&s_guard, &s_decoder);  // uses AudioOutput* ctor → notification-aware
static StreamCopy         s_copier(s_i2s, s_out);
static File               s_file;

static bool    s_active    = false;    // Do we have an active I2S.begin?
static bool    s_busy      = false;    // Are we playing a local file?
static bool    s_muted     = false;
static bool    s_paused    = false;
static volatile bool s_inCopy = false;
static uint8_t s_volumePct = 100;
static TaskHandle_t s_audioTask = nullptr;

static bool     s_isWaitTone = false;
static uint8_t  s_waitVolume = 15; // default percentage of master volume

enum class I2SOwner {
  NONE,
  LOCAL,
  BT,
  NET
};
static I2SOwner s_owner = I2SOwner::NONE;

bool audioAmpDetected() { return TAS5828M::isDetected(); }

void audioMute(bool on) {
  s_muted = on;
  uint8_t actualVol = s_isWaitTone ? (s_volumePct * s_waitVolume / 100) : s_volumePct;
  TAS5828M::setVolumeAndMute(actualVol, s_muted, false);
}

bool audioIsMuted() {
  return s_muted;
}

void audioSetVolume(uint8_t percent) {
  s_volumePct = percent;
  uint8_t actualVol = s_isWaitTone ? (s_volumePct * s_waitVolume / 100) : s_volumePct;
  TAS5828M::setVolumeAndMute(actualVol, s_muted, false);
}

void audioEnsurePlaybackPath() {
  TAS5828M::ensurePlaybackPath();
  uint8_t actualVol = s_isWaitTone ? (s_volumePct * s_waitVolume / 100) : s_volumePct;
  TAS5828M::setVolumeAndMute(actualVol, s_muted, true);
}

// -----------------------------------------------------------------------------
// Task to move data from file -> I2S (local/MP3 only)
// -----------------------------------------------------------------------------
static volatile bool s_teardownLocalTask = false;

static void audioTask(void*) {
  const TickType_t d = pdMS_TO_TICKS(2);
  for (;;) {
    if (s_teardownLocalTask) {
      s_audioTask = nullptr;
      vTaskDelete(NULL); // Task safely deletes itself
    }

    s_inCopy = true;
    bool do_copy = s_busy && !s_paused;
    int r = 1;
    if (do_copy) {
      r = s_copier.copy();
    }
    s_inCopy = false;
      
    // Only stop if the file naturally ended and another task didn't already stop it
    if (do_copy && r <= 0 && s_busy) {
      audioStop(); // Ensure decoder memory is freed when file naturally finishes
    }
    vTaskDelay(d);
  }
}

// -----------------------------------------------------------------------------
// Public info
// -----------------------------------------------------------------------------
bool audioIsPlaying() {
  return s_busy && !s_paused;
}

// -----------------------------------------------------------------------------
// Init - state only, not I2S
// -----------------------------------------------------------------------------
void audioInit() {
  File f = SD.open("/system/sound/max_volumes.txt", FILE_READ);
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.startsWith("WAIT_VOL_MAX")) {
        String valStr = line.substring(line.indexOf(' '));
        valStr.trim();
        s_waitVolume = (uint8_t)valStr.toInt();
        Serial.printf("[Audio] Loaded WAIT_VOL_MAX: %d%%\n", s_waitVolume);
      }
    }
    f.close();
  }

  TAS5828M::init();
  Serial.println("[Audio] Service initialized");
}

// -----------------------------------------------------------------------------
// Acquire/release LOCAL
// -----------------------------------------------------------------------------
bool audioAcquireLocal() {
  if (s_owner == I2SOwner::LOCAL)
    return true;

  // If owned by BT/NET, deny access
  if (s_owner == I2SOwner::BT || s_owner == I2SOwner::NET)
    return false;

  if (!s_active) {
    auto cfg = s_i2s.defaultConfig();
    cfg.copyFrom(AudioInfo(44100, 2, 16));
    cfg.pin_bck   = PIN_I2S_BCK;
    cfg.pin_ws    = PIN_I2S_LRCK;
    cfg.pin_data  = PIN_I2S_DATA;
    cfg.buffer_size  = 1024;
    cfg.buffer_count = 16;

    Serial.println("[Audio] Starting I2S.");
    s_i2s.begin(cfg);
    s_active = true;

    // Ensure amp is awake and fault-cleared after I2S starts providing clocks
    audioEnsurePlaybackPath();

    if (!s_audioTask) {
      xTaskCreatePinnedToCore(audioTask, "audioTask", 4096, nullptr, 3, &s_audioTask, 1);
    }
  }

  s_owner = I2SOwner::LOCAL;
  return true;
}

void audioReleaseLocal() {
  if (s_owner != I2SOwner::LOCAL)
    return;

  // Politely ask the task to shut itself down
  s_teardownLocalTask = true;
  s_busy = false;
  
  // Wait up to 500ms for it to cleanly exit and delete itself
  uint32_t t0 = millis();
  while (s_audioTask != nullptr && (millis() - t0) < 500) {
    delay(5);
  }
  delay(10); // Extra safety margin for FreeRTOS task cleanup
  s_teardownLocalTask = false;

  audioStop(); // Now safely close files and clear memory
  s_i2s.end();
  s_active = false;
  s_owner  = I2SOwner::NONE;
}

// -----------------------------------------------------------------------------
// Acquire/release BT (Your existing BT logic fits here)
// -----------------------------------------------------------------------------
bool audioAcquireBt() {
  // Release potential local/net hold
  if (s_owner == I2SOwner::LOCAL) {
    audioReleaseLocal();
  } else if (s_owner == I2SOwner::NET) {
    audioReleaseNet();
  }

  s_owner = I2SOwner::BT;
  return true;
}

void audioReleaseBt() {
  if (s_owner == I2SOwner::BT) {
    s_owner = I2SOwner::NONE;
  }
}

// -----------------------------------------------------------------------------
// Acquire/release NET - same driver as LOCAL
// -----------------------------------------------------------------------------
bool audioAcquireNet() {
  if (s_owner == I2SOwner::NET)
    return true;

  if (s_owner == I2SOwner::LOCAL) {
    audioReleaseLocal();
  } else if (s_owner == I2SOwner::BT) {
    audioReleaseBt();
  }

  // Start I2S if not active
  if (!s_active) {
    auto cfg = s_i2s.defaultConfig();
    cfg.copyFrom(AudioInfo(44100, 2, 16));
    cfg.pin_bck   = PIN_I2S_BCK;
    cfg.pin_ws    = PIN_I2S_LRCK;
    cfg.pin_data  = PIN_I2S_DATA;
    cfg.buffer_size  = 1024;
    cfg.buffer_count = 16;

    Serial.println("[Audio] Starting I2S (net).");
    s_i2s.begin(cfg);
    s_active = true;

    // Ensure amp is awake and fault-cleared after I2S starts providing clocks
    audioEnsurePlaybackPath();

  }

  s_owner = I2SOwner::NET;
  return true;
}

void audioReleaseNet() {
  if (s_owner != I2SOwner::NET)
    return;

  s_i2s.end();
  s_active = false;
  s_owner  = I2SOwner::NONE;
}

// -----------------------------------------------------------------------------
// Play/stop (lokal/MP3)
// -----------------------------------------------------------------------------
void audioPlay(const char* path) {
  if (!audioAcquireLocal()) {
    Serial.println("[Audio] can't acquire I2S for local playback");
    return;
  }

  if (s_busy) {
    audioStop(); // Cleanly shutdown previous playback and free decoder memory
  }

  // Check if it's the wait tone to override volume
  if (path != nullptr && strcmp(path, "/system/sound/wait.mp3") == 0) {
    s_isWaitTone = true;
    Serial.printf("[Audio] Wait tone volume override: %d%% of master\n", s_waitVolume);
  } else {
    s_isWaitTone = false;
  }

  s_file = SD.open(path, FILE_READ);
  if (!s_file) {
    Serial.printf("[Audio] Failed to open %s\n", path);
    s_isWaitTone = false; // Reset just in case
    return;
  }

  s_out.begin(AudioInfo(44100, 2, 16));
  s_copier.begin(s_out, s_file);
  s_busy   = true;
  s_paused = false;
  audioMute(false);

  Serial.printf("[Audio] PLAY %s\n", path);
}

void audioStop() {
  bool was_busy = s_busy;
  s_busy = false;
  s_paused = false;

  if (was_busy) {
    uint32_t t0 = millis();
    // Wait until the audioTask has safely exited the copy block
    while (s_inCopy && (millis() - t0) < 150) {
      delay(2);
    }
  }

  if (s_file) s_file.close();

  if (was_busy) {
    s_out.end(); // Only free local MP3 decoder memory if it was actually running
  }
  audioMute(true);

  if (s_isWaitTone) {
    s_isWaitTone = false;
  }
  Serial.println("[Audio] STOP");
}

// -----------------------------------------------------------------------------
// Pause/resume
// -----------------------------------------------------------------------------
void audioPause() {
  if (s_busy) {
    audioMute(true);
    s_paused = true;
    Serial.println("[Audio] PAUSE");
  }
}

void audioResume() {
  if (s_busy && s_paused) {
    s_paused = false;
    audioMute(false);
    Serial.println("[Audio] RESUME");
  }
}

bool audioIsPaused() {
  return s_paused;
}

// -----------------------------------------------------------------------------
// Mode helpers
// -----------------------------------------------------------------------------
bool audioStartMp3Mode() { return audioAcquireLocal(); }
bool audioStartBtMode()  { return audioAcquireBt(); }
bool audioStartNetMode() { return audioAcquireNet(); }

// -----------------------------------------------------------------------------
// Hand over the guarded output to Net mode
// -----------------------------------------------------------------------------
audio_tools::AudioOutput& audioGetOutput() {
  return s_guard;
}

// -----------------------------------------------------------------------------
// Force shutdown (if used elsewhere)
// -----------------------------------------------------------------------------
void audioForceShutdown() {
  if (s_owner == I2SOwner::LOCAL) {
    s_teardownLocalTask = true;
    s_busy = false;
    uint32_t t0 = millis();
    while (s_audioTask != nullptr && (millis() - t0) < 500) delay(5);
    s_teardownLocalTask = false;
  }

  audioStop();
  s_i2s.end();
  s_active = false;
  s_owner  = I2SOwner::NONE;
  Serial.println("[Audio] force shutdown");
}
