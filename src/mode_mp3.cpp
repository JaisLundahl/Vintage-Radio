// mode_mp3.cpp - Number-based shuffle with shared state file
#include "mode_mp3.h"

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <vector>

#include "pins.h"
#include "AdcService.h"
#include "AudioService.h"   // AudioService ejer I2S
// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------
static constexpr uint8_t  kNumFolders        = 16;
static constexpr char     kBasePrefix[]      = "/folder";
static constexpr char     kConfigPath[]      = "/system/mp3/mp3_folders.txt";
static constexpr char     kStateFilePath[]   = "/system/mp3/mp3_state.txt";
static constexpr char     kWaitTonePath[]    = "/system/sound/wait.mp3";
static constexpr uint8_t  kDigits            = 8;     // 00000001.mp3
static constexpr uint8_t  kHistorySize       = 16;    // Max history items per folder
static constexpr size_t   kMaxPathLen        = 128;   // Max length for file paths

static constexpr uint32_t kTaskTickMs        = 50;    // Task polling interval
static constexpr uint32_t kIgnoreTunerBootMs = 800;   // Ignore tuner at boot
static constexpr uint32_t kIgnoreTunerSkipMs = 1500;  // Ignore tuner after Next/Prev
static constexpr uint32_t kTuneConfirmMs     = 1500;  // Time to hold tuner before selecting station
static constexpr uint32_t kMaxSearchDepth    = 20;    // Files to look backwards if missing
static constexpr uint32_t kMaxWriteWaitMs    = 200;   // Max wait time for state file writing

// tuner/debounce
static uint32_t           s_ignoreTunerUntil = 0;
static uint32_t           s_lastTuneMs       = 0;
static int                s_pendingBin       = 0;

// -----------------------------------------------------------------------------
// Per-folder state
// -----------------------------------------------------------------------------
struct FolderState {
  String   path;
  uint32_t lastIdx;
  uint32_t maxIdx;
  std::vector<uint32_t> history;
  int      history_pos;
};

static FolderState  s_folders[kNumFolders];
static bool         s_stateLoaded    = false;
static int8_t       s_currentFolder  = -1;
static int8_t       s_lastBin        = -1;

static bool         s_playing        = false;
static bool         s_paused         = false;
static TaskHandle_t s_task           = nullptr;
static volatile bool s_killTask      = false;
static char         s_currentPath[kMaxPathLen] = {0};

// NEW: used to ensure SD isn't unmounted during writes
static volatile bool s_writingState  = false;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static inline String normalizeFolder(const String& raw) {
  String s = raw; s.trim();
  while (s.length() > 1 && s.endsWith("/")) s.remove(s.length()-1);
  if (!s.length()) return "";
  if (s[0] != '/') s = "/" + s;
  return s;
}

static void playWaitTone() {
  if (strncmp(s_currentPath, kWaitTonePath, kMaxPathLen) == 0)
    return;

  Serial.println("[MP3] WAIT: tuning...");
  s_playing     = false;
  strncpy(s_currentPath, kWaitTonePath, kMaxPathLen);

  audioStop();
  audioPlay(kWaitTonePath);
}

static void setDefaultFolderNames() {
  for (uint8_t i = 0; i < kNumFolders; i++) {
    s_folders[i].path    = String(kBasePrefix) + String(i + 1);
    s_folders[i].lastIdx = 1;
    s_folders[i].maxIdx  = 1;
    s_folders[i].history.clear();
    s_folders[i].history_pos = -1;
  }
}

static void loadFolderNamesFromConfig() {
  setDefaultFolderNames();
  if (!SD.exists(kConfigPath)) {
    Serial.printf("[MP3] No config at %s – using defaults.\n", kConfigPath);
    return;
  }
  File f = SD.open(kConfigPath, FILE_READ);
  if (!f) return;

  uint8_t autoIdx = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    if (!line.length()) continue;
    if (line[0] == '#' || line[0] == ';') continue;

    String norm = normalizeFolder(line);
    if (!norm.length()) continue;
    if (autoIdx < kNumFolders) {
      s_folders[autoIdx].path = norm;
      autoIdx++;
    }
  }
  f.close();
}

static void buildFilePath(uint8_t idx, uint32_t fileIdx, char* outBuf, size_t maxLen) {
  snprintf(outBuf, maxLen, "%s/%0*u.mp3", s_folders[idx].path.c_str(), kDigits, (unsigned)fileIdx);
}

// -----------------------------------------------------------------------------
// One-time indexing of a single folder
// -----------------------------------------------------------------------------
static uint32_t discoverMaxIndexForFolder(const String& path) {
  if (!SD.exists(path)) return 1;

  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 1;
  }

  uint32_t maxIdx = 1;
  int fileCount = 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      const char* name = f.name();
      const char* dot = strrchr(name, '.');
      if (dot != nullptr) {
        size_t len = dot - name;
        if (len == kDigits) {
          uint32_t v = 0;
          bool valid = true;
          for (size_t i = 0; i < len; i++) {
            if (name[i] >= '0' && name[i] <= '9') {
              v = v * 10 + (name[i] - '0');
            } else {
              valid = false;
              break;
            }
          }
          if (valid && v > maxIdx) maxIdx = v;
        }
      }
    }
    f.close();
    
    // Feed the watchdog to prevent unintended reboots on large folders
    if (++fileCount % 32 == 0) {
      delay(1);
    }
  }
  dir.close();
  Serial.printf("[MP3] discover %s -> max %u\n", path.c_str(), (unsigned)maxIdx);
  return maxIdx;
}

// -----------------------------------------------------------------------------
// State-file I/O (shared for all 16 folders)
// -----------------------------------------------------------------------------
static void writeGlobalStateFile() {
  if (!SD.exists("/system")) SD.mkdir("/system");
  if (!SD.exists("/system/mp3")) SD.mkdir("/system/mp3");

  s_writingState = true;  // ← NEW: signal that writing is in progress
  File f = SD.open(kStateFilePath, FILE_WRITE);
  if (!f) {
    Serial.println("[MP3] Cannot write global state file");
    s_writingState = false;
    return;
  }
  for (uint8_t i = 0; i < kNumFolders; i++) {
    f.printf("%s %u %u %d",
             s_folders[i].path.c_str(),
             (unsigned)s_folders[i].lastIdx,
             (unsigned)s_folders[i].maxIdx,
             s_folders[i].history_pos);
    if (s_folders[i].history.size() > 0) {
        f.print(" ");
        for (size_t j = 0; j < s_folders[i].history.size(); ++j) {
            f.print(s_folders[i].history[j]);
            if (j < s_folders[i].history.size() - 1) {
                f.print(",");
            }
        }
    }
    f.print("\n");
  }
  f.close();
  s_writingState = false; // ← NEW
  Serial.println("[MP3] Global state file written");
}

static void initOrLoadGlobalState() {
  loadFolderNamesFromConfig();

  bool needs_reindex = true;
  if (SD.exists(kStateFilePath)) {
    File f = SD.open(kStateFilePath, FILE_READ);
    if (f) {
      needs_reindex = false; // Assume success unless parsing fails
      while (f.available()) {
        String line = f.readStringUntil('\n'); line.trim();
        if (!line.length()) continue;

        // Format: /path lastIdx maxIdx history_pos hist1,hist2,hist3...
        int p_path = line.indexOf(' ');
        if (p_path <= 0) { needs_reindex = true; break; }
        String path = line.substring(0, p_path);

        int p_last = line.indexOf(' ', p_path + 1);
        if (p_last <= p_path) { needs_reindex = true; break; }
        uint32_t last = (uint32_t)line.substring(p_path + 1, p_last).toInt();

        int p_max = line.indexOf(' ', p_last + 1);
        if (p_max <= p_last) { needs_reindex = true; break; }
        uint32_t mx = (uint32_t)line.substring(p_last + 1, p_max).toInt();

        int p_hist_pos = line.indexOf(' ', p_max + 1);
        int history_pos;
        String history_str;
        
        if (p_hist_pos != -1) {
            history_pos = line.substring(p_max + 1, p_hist_pos).toInt();
            history_str = line.substring(p_hist_pos + 1);
            history_str.trim();
        } else {
            history_pos = line.substring(p_max + 1).toInt();
            history_str = "";
        }

        bool found_folder = false;
        for (uint8_t i = 0; i < kNumFolders; i++) {
          if (s_folders[i].path == path) {
            s_folders[i].lastIdx = (last > 0) ? last : 1;
            s_folders[i].maxIdx  = (mx > 0) ? mx : 1;
            s_folders[i].history_pos = history_pos;
            
            s_folders[i].history.clear();
            int current_pos = 0;
            while(history_str.length() > 0 && current_pos < history_str.length()) {
                int comma_pos = history_str.indexOf(',', current_pos);
                if (comma_pos == -1) {
                    comma_pos = history_str.length();
                }
                String val_str = history_str.substring(current_pos, comma_pos);
                if (val_str.length() > 0) {
                    s_folders[i].history.push_back((uint32_t)val_str.toInt());
                }
                current_pos = comma_pos + 1;
            }
            found_folder = true;
            break;
          }
        }
      }
      f.close();
      if (!needs_reindex) {
          s_stateLoaded = true;
          Serial.println("[MP3] Global state file loaded");
          return;
      }
    }
  }

  // If we reach this point -> no state file exists
  Serial.println("[MP3] Global state missing or invalid – scanning all folders...");
  for (uint8_t i = 0; i < kNumFolders; i++) {
    uint32_t mx = discoverMaxIndexForFolder(s_folders[i].path);
    s_folders[i].maxIdx  = mx;
    s_folders[i].lastIdx = 1;
    s_folders[i].history.clear();
    s_folders[i].history_pos = -1;
  }
  writeGlobalStateFile();
  s_stateLoaded = true;
}

// -----------------------------------------------------------------------------
// Player (control - playback is handled in AudioService)
// -----------------------------------------------------------------------------
static void playerStop() {
  if (s_playing) {
    Serial.printf("[MP3] STOP %s\n", s_currentPath);
  }
  audioStop();
  s_playing     = false;
  s_currentPath[0] = '\0';
}

static bool playerStart(const char* fullPath) {
  if (!SD.exists(fullPath)) {
    Serial.printf("[MP3] File missing: %s\n", fullPath);
    return false;
  }

  Serial.printf("[MP3] PLAY %s\n", fullPath);
  strncpy(s_currentPath, fullPath, kMaxPathLen);
  audioPlay(fullPath);
  s_playing = true;
  s_paused  = false;
  return true;
}

// -----------------------------------------------------------------------------
// Select and play
// -----------------------------------------------------------------------------
static bool playTrackAndUpdateState(uint8_t folderIdx, uint32_t fileIdx, bool isNewRandom); // Fwd decl

static uint32_t pickRandomIndex(uint32_t maxIdx, uint32_t lastIdx) {
  if (maxIdx <= 1) return 1;
  uint32_t r = esp_random();
  uint32_t choice = (r % maxIdx) + 1;
  if (choice == lastIdx && maxIdx > 1) {
    choice = (choice % maxIdx) + 1;
  }
  return choice;
}

static bool playTrackAndUpdateState(uint8_t folderIdx, uint32_t fileIdx, bool isNewRandom) {
  char fullPath[kMaxPathLen];
  uint32_t finalFileIdx = 0;

  // First, try the requested index
  buildFilePath(folderIdx, fileIdx, fullPath, sizeof(fullPath));
  if (SD.exists(fullPath)) {
    finalFileIdx = fileIdx;
  } else {
    // Fallback: search backwards if the file is missing
    Serial.printf("[MP3] File %u missing, searching backwards...\n", (unsigned)fileIdx);
    for (int32_t i = (int32_t)fileIdx - 1; i >= 1 && i >= (int32_t)fileIdx - (int32_t)kMaxSearchDepth; --i) {
      buildFilePath(folderIdx, (uint32_t)i, fullPath, sizeof(fullPath));
      if (SD.exists(fullPath)) {
        finalFileIdx = (uint32_t)i;
        Serial.printf("[MP3] Found fallback file %u\n", (unsigned)finalFileIdx);
        break;
      }
    }
  }

  if (finalFileIdx == 0) {
    Serial.printf("[MP3] No playable file found near index %u in %s\n", (unsigned)fileIdx, s_folders[folderIdx].path.c_str());
    return false;
  }

  // Now play the found file
  buildFilePath(folderIdx, finalFileIdx, fullPath, sizeof(fullPath));
  playerStop();
  if (playerStart(fullPath)) {
    s_folders[folderIdx].lastIdx = finalFileIdx;
    if (finalFileIdx > s_folders[folderIdx].maxIdx) {
        s_folders[folderIdx].maxIdx = finalFileIdx;
    }

    if (isNewRandom) {
        auto& hist = s_folders[folderIdx].history;
        // If we were browsing history, truncate it from the current position
        if (s_folders[folderIdx].history_pos != -1 && s_folders[folderIdx].history_pos < (int)hist.size() - 1) {
            hist.erase(hist.begin() + s_folders[folderIdx].history_pos + 1, hist.end());
        }
        
        hist.push_back(finalFileIdx);
        if (hist.size() > kHistorySize) {
            hist.erase(hist.begin());
        }
        // The new "current" position is the end of the list
        s_folders[folderIdx].history_pos = hist.size() - 1;
    }
    
    writeGlobalStateFile();
    return true;
  }
  return false;
}

// -----------------------------------------------------------------------------
// Task - Tuner -> Folder
// -----------------------------------------------------------------------------
static void playRandomInFolder(uint8_t idx); // Fwd decl

static void mp3Task(void*) {
  const TickType_t tick = pdMS_TO_TICKS(kTaskTickMs);

  for (;;) {
    if (s_killTask) {
      s_task = nullptr;
      vTaskDelete(NULL); // Safely deletes itself
    }

    uint32_t now = millis();

    // Keep the wait tone playing seamlessly if we are stuck between stations
    if (strncmp(s_currentPath, kWaitTonePath, kMaxPathLen) == 0 && !audioIsPlaying()) {
      audioPlay(kWaitTonePath);
    }

    // If we just pressed next, ignore tuner input briefly
    if (now < s_ignoreTunerUntil) {
      vTaskDelay(tick);
      continue;
    }

    int bin = adcTunerBin16();

    if (!s_paused) {
      // 1) Register change
      if (bin != s_pendingBin) {
        s_pendingBin = bin;
        s_lastTuneMs = now;
        playWaitTone();
      }

      // 2) Confirm after ~1.5s
      if ((now - s_lastTuneMs) > kTuneConfirmMs) {
        if (s_pendingBin == 0) {
          if (s_lastBin != 0) {
            s_lastBin = 0;
            Serial.println("[MP3] WAIT: out of range");
            playWaitTone();
          }
        } else if (s_pendingBin != s_lastBin) {
          s_lastBin = s_pendingBin;
          Serial.printf("[MP3] Confirmed bin %d\n", s_lastBin);
          playRandomInFolder((uint8_t)(s_lastBin - 1));
        } else {
          // <-- Here we are locked onto the same "station"
          if (!audioIsPlaying() && s_currentFolder >= 0) {
            // File finished -> play next in the same folder
            modeMp3Next();
          }
        }
      }
    }

    vTaskDelay(tick);
  }
}

static void playRandomInFolder(uint8_t idx) {
  if (idx >= kNumFolders || !s_stateLoaded) return;

  uint32_t maxIdx  = s_folders[idx].maxIdx;
  uint32_t lastIdx = s_folders[idx].lastIdx;

  uint32_t target  = (maxIdx <= 1)
                     ? 1
                     : pickRandomIndex(maxIdx, lastIdx);

  (void)playTrackAndUpdateState(idx, target, true); // true for isNewRandom
  s_currentFolder = idx;
}



// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

/**
 * @brief Starts the MP3 Mode and background tasks.
 * @note This hijacks the I2S hardware and plays standard folders from the SD card.
 */
void modeMp3Start() {
  if (!audioStartMp3Mode()) {
    Serial.println("[MP3] could not acquire I2S (probably BT owns it)");
    return;
  }

  initOrLoadGlobalState();

  if (s_task) {
    s_killTask = true;
    while(s_task != nullptr) { delay(10); } // Wait for task to safely exit
  }
  s_killTask = false;

  int bin = adcTunerBin16();
  s_lastBin     = bin;
  s_pendingBin  = bin;
  s_lastTuneMs  = millis();
  s_ignoreTunerUntil = millis() + kIgnoreTunerBootMs;

  if (bin == 0) {
    audioPlay(kWaitTonePath);
    strncpy(s_currentPath, kWaitTonePath, kMaxPathLen);
    s_playing = true;
  } else {
    playRandomInFolder((uint8_t)(bin - 1));
  }

  xTaskCreatePinnedToCore(mp3Task, "mp3Task", 4096, nullptr, 3, &s_task, 1);
}

/**
 * @brief Stops MP3 mode gracefully.
 * @note Safely shuts down the FreeRTOS task and ensures SD card buffers are written.
 */
void modeMp3Stop() {
  // 1) Stop task first to prevent new writes
  if (s_task) {
    s_killTask = true;
    while(s_task != nullptr) { delay(10); } // Wait for task to safely exit
  }

  // 2) Wait briefly if caught mid-write
  uint32_t t0 = millis();
  while (s_writingState && (millis() - t0) < kMaxWriteWaitMs) {
    delay(5);
  }

  // 3) Stop audio
  playerStop();

  s_currentFolder = -1;
  s_paused        = false;

  // 4) Release I2S for the rest of the system
  audioReleaseLocal();

  Serial.println("[MP3] stop");
}

/**
 * @brief Selects the next random song in the current folder.
 */
void modeMp3Next() {
  if (s_currentFolder < 0 || s_paused) return;
  
  s_ignoreTunerUntil = millis() + kIgnoreTunerSkipMs;

  auto& folder = s_folders[s_currentFolder];
  
  // Are we in history playback and not at the end?
  if (folder.history_pos >= 0 && folder.history_pos < (int)folder.history.size() - 1) {
    folder.history_pos++;
    uint32_t track_idx = folder.history[folder.history_pos];
    Serial.printf("[MP3] History NEXT -> pos %d, track %u\n", folder.history_pos, (unsigned)track_idx);
    playTrackAndUpdateState((uint8_t)s_currentFolder, track_idx, false);
  } else {
    // We are at the end of history, play a new random track
    Serial.println("[MP3] History end, playing new random track");
    playRandomInFolder((uint8_t)s_currentFolder);
  }
}

/**
 * @brief Plays the previously played song in the current folder.
 */
void modeMp3Prev() {
  if (s_currentFolder < 0 || s_paused) return;

  s_ignoreTunerUntil = millis() + kIgnoreTunerSkipMs;

  auto& folder = s_folders[s_currentFolder];

  // Can we go back in history?
  if (folder.history_pos > 0) {
    folder.history_pos--;
    uint32_t track_idx = folder.history[folder.history_pos];
    Serial.printf("[MP3] History PREV -> pos %d, track %u\n", folder.history_pos, (unsigned)track_idx);
    playTrackAndUpdateState((uint8_t)s_currentFolder, track_idx, false);
  } else {
    Serial.println("[MP3] At start of history, cannot go back further.");
    // Optional: maybe flash an LED or give some feedback
  }
}

/**
 * @brief Pauses or resumes the current playback.
 */
void modeMp3PlayPauseToggle() {
  if (!s_playing) return;

  s_paused = !s_paused;
  if (s_paused) {
    audioPause();          // <-- instead of audioMute(true)
    Serial.println("[MP3] PAUSED");
  } else {
    audioResume();         // <-- instead of audioMute(false)
    Serial.println("[MP3] RESUMED");
  }
}