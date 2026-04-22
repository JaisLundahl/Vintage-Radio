#include "mode_net.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <string.h>

#include "AdcService.h"
#include "AudioService.h"

#include <AudioTools.h>
#include <AudioTools/AudioCodecs/CodecMP3Helix.h>
#include <AudioTools/AudioCodecs/CodecAACHelix.h>
using namespace audio_tools;

// ----------------------------------------------------
// Configuration & Constants
// ----------------------------------------------------
static constexpr uint8_t  kMaxStations       = 16;      // Max number of stations to load from stations.txt
static constexpr uint8_t  kMaxWiFiCreds      = 16;      // Max number of Wi-Fi credentials to load from WIFI.txt
static constexpr uint32_t kHttpTimeoutMs     = 3500;    // Timeout for HTTP connection attempts (kept short to prevent UI blocking)
static constexpr uint32_t kWiFiTimeoutMs     = 12000;   // Max time to wait for the Wi-Fi connection to establish
static constexpr uint32_t kTaskTickMs        = 2;       // RTOS task delay interval (very short to keep audio buffers full)
static constexpr uint32_t kStreamStallMs     = 10000;   // Watchdog timeout to consider a stream dead if no data is received
static constexpr uint32_t kStreamRetryMs     = 15000;   // Time to wait before attempting to reconnect to a failed stream
static constexpr uint32_t kTuneConfirmMs     = 1500;    // Time to hold the tuner knob steady before committing to a station change
static constexpr uint32_t kIgnoreTunerBootMs = 800;     // Time to ignore analog tuner noise right after the device boots
static constexpr uint32_t kIgnoreTunerWifiMs = 15000;   // Time to safely ignore the tuner while the system blocks to connect to Wi-Fi
static constexpr uint32_t kRxPrintIntervalMs = 1000;    // Interval to log the stream download speed (bytes/sec) to the Serial monitor
static constexpr char     kWaitTonePath[]    = "/system/sound/wait.mp3"; // SD card path to the wait tone

// ----------------------------------------------------
// Simple station structure
// ----------------------------------------------------
struct Station { String name, url, type; };

// ----------------------------------------------------
// Global state
// ----------------------------------------------------
static Station  s_st[kMaxStations];
static uint8_t  s_stCount         = 0;
static int8_t   s_index           = -1;
static int8_t   s_wantedIndex     = -1;
static int8_t   s_lastBin         = -1;
static int      s_pendingBin      = -1;
static uint32_t s_lastTuneMs      = 0;
static uint32_t s_ignoreTunerUntil= 0;
static bool     s_playingWaitTone = false;

static volatile bool s_cmdStop    = false;
static volatile bool s_cmdPlay    = false;

static bool     s_wifiOk          = false;
static bool     s_needResumePlay  = false;

static bool     s_playing         = false;
static uint32_t s_lastAudioData   = 0;
static volatile bool s_inNetCopy  = false;
static bool     s_needStreamRetry = false;
static uint32_t s_lastStreamAttempt = 0;

static uint32_t s_lastRxPrint     = 0;
static size_t   s_rxBytesSec      = 0;

// HTTP / net
static HTTPClient* s_http   = nullptr;
static WiFiClient* s_client = nullptr;

// Decoders (lazy initialization)
static MP3DecoderHelix     s_mp3;
static AACDecoderHelix     s_aac;

static EncodedAudioStream* s_decMp3 = nullptr;
static EncodedAudioStream* s_decAac = nullptr;
static EncodedAudioStream* s_curDec = nullptr;
static StreamCopy*         s_copy   = nullptr;
static TaskHandle_t        s_task   = nullptr;
static volatile bool       s_killTask = false;

// Forward declaration of the FreeRTOS task
static void netTask(void* arg);

// ----------------------------------------------------
// Helper functions
// ----------------------------------------------------
static bool endsWithIgnoreCase(const String &s, const char *suffix) {
  int sl = s.length(); int tl = strlen(suffix);
  if (tl > sl) return false;
  return s.substring(sl - tl).equalsIgnoreCase(suffix);
}

static bool parsePlsBody(const String &body, String &outUrl) {
  int from = 0;
  while (from < body.length()) {
    int nl = body.indexOf('\n', from);
    if (nl < 0) nl = body.length();
    String line = body.substring(from, nl);
    line.trim();
    if (line.startsWith("File") || line.startsWith("file")) {
      int eq = line.indexOf('=');
      if (eq > 0) {
        outUrl = line.substring(eq+1); outUrl.trim();
        return outUrl.length();
      }
    }
    from = nl + 1;
  }
  return false;
}

static String fetchM3uUrl(const String& m3uUrl) {
  HTTPClient hc;
  WiFiClient* wc = nullptr;
  if (m3uUrl.startsWith("https")) {
    WiFiClientSecure* sec = new WiFiClientSecure();
    sec->setInsecure();
    wc = sec;
  } else {
    wc = new WiFiClient();
  }

  hc.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  hc.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/115.0.0.0 Safari/537.36");
  
  const char* hdrs[] = {"Location"};
  hc.collectHeaders(hdrs, 1);

  hc.begin(*wc, m3uUrl);
  int code = hc.GET();
  if (code == 301 || code == 302 || code == 307 || code == 308) {
    String loc = hc.header("Location");
    hc.end();
    delete wc;
    if (loc.length()) return fetchM3uUrl(loc);
    return "";
  }

  String body = (code == 200) ? hc.getString() : "";
  hc.end();
  delete wc;
  if (body.length() == 0) return "";

  int from = 0;
  while (from < body.length()) {
    int nl = body.indexOf('\n', from);
    if (nl < 0) nl = body.length();
    String line = body.substring(from, nl);
    line.trim();
    if (line.length() && !line.startsWith("#"))
      return line;
    from = nl + 1;
  }
  return "";
}

// ----------------------------------------------------
// Load stations from SD card
// ----------------------------------------------------
static bool loadStations(const char* path) {
  s_stCount = 0;
  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.printf("[NET] stations open fail: %s\n", path);
    return false;
  }
  while (f.available() && s_stCount < kMaxStations) {
    String line = f.readStringUntil('\n'); line.trim();
    if (!line.length() || line.startsWith("#")) continue;
    int p1 = line.indexOf('|'); int p2 = line.indexOf('|', p1+1);
    if (p1 < 0 || p2 < 0) continue;
    s_st[s_stCount].name = line.substring(0, p1);
    s_st[s_stCount].url  = line.substring(p1+1, p2);
    s_st[s_stCount].type = line.substring(p2+1);
    s_stCount++;
  }
  f.close();
  Serial.printf("[NET] stations: %u\n", s_stCount);
  return s_stCount > 0;
}

// ----------------------------------------------------
// Stop audio playback
// ----------------------------------------------------
static void stopAudio(){
  s_playing = false; // Tell task to stop copying
  uint32_t t0 = millis();
  // Wait until the netTask has safely exited the copy block
  while (s_inNetCopy && (millis() - t0) < 150) {
    delay(2);
  }

  if (s_copy) {
    delete s_copy;
    s_copy = nullptr;
  }

  if (s_curDec) {
    s_curDec->end();
    s_curDec = nullptr; // Reset the pointer to prevent double-free crashes
  }

  if (s_http) {
    s_http->end();
    delete s_http;
    s_http = nullptr;
  }

  if (s_client) {
    s_client->stop();
    delete s_client;
    s_client = nullptr;
  }
}

// fwd
static void playWaitTone() {
  if (!s_playingWaitTone) {
    Serial.println("[NET] WAIT: tuning...");
    stopAudio();          // Stop the network stream
    audioReleaseNet();    // Give up I2S ownership to LOCAL
    s_playingWaitTone = true;
  }
}

// fwd
static bool playUrl(const String& url);

static bool isRandom(const Station& st){
  return st.type.equalsIgnoreCase("random") || st.url.startsWith("random://");
}

static bool fetchRandomAndPlay(const String&) {
  if (s_stCount == 0) return false;
  for (uint8_t tries=0; tries<8; ++tries) {
    int r = random(s_stCount);
    if (!isRandom(s_st[r])) return playUrl(s_st[r].url);
  }
  for (uint8_t i=0; i<s_stCount; i++)
    if (!isRandom(s_st[i])) return playUrl(s_st[i].url);
  return false;
}

// ----------------------------------------------------
// playUrl
// ----------------------------------------------------
static bool playUrl(const String& url) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[AUD] no wifi -> cannot play");
    return false;
  }

  Serial.printf("[AUD] play: %s\n", url.c_str());

  // Close previous connection
  stopAudio();

  if (url.startsWith("https")) {
    WiFiClientSecure* sec = new WiFiClientSecure();
    sec->setInsecure();
    s_client = sec;
  } else {
    s_client = new WiFiClient();
  }

  if (!s_http) {
    s_http = new HTTPClient();
  }

  s_http->setTimeout(kHttpTimeoutMs); // Shorter timeout prevents tuning block
  s_http->setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS); // Manual follow to allow HTTP->HTTPS switching
  s_http->addHeader("Icy-MetaData", "0");
  s_http->setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/115.0.0.0 Safari/537.36");

  static const char* wantedHeaders[] = {"Content-Type", "content-type", "Location"};
  s_http->collectHeaders(wantedHeaders, 3);

  if (!s_http->begin(*s_client, url)) {
    stopAudio();
    return false;
  }

  int code = s_http->GET();
  Serial.printf("[HTTP] GET -> %d\n", code);

  // Handle redirects manually to upgrade WiFiClient to WiFiClientSecure if needed
  if (code == 301 || code == 302 || code == 307 || code == 308) {
    String loc = s_http->header("Location");
    stopAudio();
    if (loc.length()) {
      Serial.printf("[HTTP] redirect -> %s\n", loc.c_str());
      return playUrl(loc);
    }
    return false;
  }

  if (code < 0 || code == 401 || code == 403 || code == 404) {
    stopAudio();
    return false;
  }

  if (!(code == 200 || code == 206)) {
    stopAudio();
    return false;
  }

  String ctype = s_http->header("Content-Type");
  if (!ctype.length())
    ctype = s_http->header("content-type");
  Serial.printf("[HTTP] Content-Type: %s\n", ctype.c_str());

  // playlist?
  bool looksPls = (ctype.indexOf("audio/x-scpls") >= 0) || endsWithIgnoreCase(url, ".pls");
  bool looksM3u = (ctype.indexOf("audio/x-mpegurl") >= 0) ||
                  endsWithIgnoreCase(url, ".m3u") ||
                  endsWithIgnoreCase(url, ".m3u8");

  if (looksPls) {
    String body = s_http->getString();
    stopAudio();
    String realUrl;
    if (parsePlsBody(body, realUrl)) {
      Serial.printf("[HTTP] .pls -> %s\n", realUrl.c_str());
      return playUrl(realUrl);
    }
    return false;
  }

  if (looksM3u) {
    stopAudio();
    String realUrl = fetchM3uUrl(url);
    if (realUrl.length()) return playUrl(realUrl);
    return false;
  }

  // Real stream handling (use the guarded output to prevent zero-sample-rate crashes)
  AudioOutput& out = audioGetOutput();

  // Lazy-initialize both decoders
  if (!s_decMp3) {
    s_decMp3 = new EncodedAudioStream(&out, &s_mp3);
  }
  if (!s_decAac) {
    s_decAac = new EncodedAudioStream(&out, &s_aac);
  }

  // Clear old stream copy
  if (s_copy) {
    delete s_copy;
    s_copy = nullptr;
  }

  bool isAac = (ctype.indexOf("aac") >= 0 || ctype.indexOf("aacp") >= 0);

  if (isAac) {
    Serial.println("[AUD] using AAC decoder (reduced)");
    s_curDec = s_decAac;

    if (!s_decAac->begin()) {
      Serial.println("[AUD] AAC begin failed (likely OOM) – aborting this stream");
      stopAudio();
      return false;
    }

    s_copy = new StreamCopy(*s_decAac, s_http->getStream());
  } else {
    Serial.println("[AUD] using MP3 decoder");
    s_curDec = s_decMp3;
    if (!s_decMp3->begin()) {
      Serial.println("[AUD] MP3 begin failed (likely OOM) – aborting this stream");
      stopAudio();
      return false;
    }
    s_copy = new StreamCopy(*s_decMp3, s_http->getStream());
  }

  s_playing           = true;
  s_lastAudioData     = millis();
  s_needStreamRetry   = false;
  s_lastStreamAttempt = millis();

  audioMute(false); // Ensure the amplifier is unmuted

  return true;
}

// ----------------------------------------------------
// Apply station selection
// ----------------------------------------------------
static void applyStation(int idx){
  if (s_playingWaitTone) {
    audioReleaseLocal();  // Release LOCAL ownership
    audioAcquireNet();    // Re-acquire NET ownership for streaming
    s_playingWaitTone = false;
  }
  if (idx<0 || idx>=s_stCount) return;
  s_index = idx;
  Station& st = s_st[idx];
  Serial.printf("[NET] station %d: %s (%s)\n", idx, st.name.c_str(), st.type.c_str());
  stopAudio();
  bool ok = false;
  if (isRandom(st)) ok = fetchRandomAndPlay(st.url);
  else              ok = playUrl(st.url);
  s_needStreamRetry   = !ok;
  s_lastStreamAttempt = millis();
}

// ----------------------------------------------------
// Load WiFi credentials from file
// ----------------------------------------------------
static bool connectWiFiFromFile() {
  File f = SD.open("/system/netradio/WIFI.txt", FILE_READ);
  if (!f) {
    Serial.println("[WiFi] no WIFI.txt");
    return false;
  }

  struct Cred { String ssid, pass; };
  Cred creds[kMaxWiFiCreds];
  int  cc = 0;
  while (f.available() && cc < kMaxWiFiCreds) {
    String line = f.readStringUntil('\n'); line.trim();
    if (!line.length() || line.startsWith("#")) continue;
    int p = line.indexOf('|');
    String ssid = (p<0)? line : line.substring(0,p);
    String pass = (p<0)? ""   : line.substring(p+1);
    ssid.trim(); pass.trim();
    if (ssid.length()) {
      creds[cc].ssid = ssid;
      creds[cc].pass = pass;
      cc++;
    }
  }
  f.close();

  Serial.println("[WiFi] scanning...");
  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) {
    Serial.println("[WiFi] no networks");
    return false;
  }

  int bestCred = -1;
  int bestRssi = -200;
  for (int i=0; i<n; i++) {
    String found = WiFi.SSID(i);
    int rssi     = WiFi.RSSI(i);
    for (int c=0; c<cc; c++) {
      if (found == creds[c].ssid && rssi > bestRssi) {
        bestRssi = rssi;
        bestCred = c;
      }
    }
  }

  if (bestCred < 0) {
    Serial.println("[WiFi] none of known APs found");
    return false;
  }

  Serial.printf("[WiFi] best match: '%s' (RSSI=%d)\n", creds[bestCred].ssid.c_str(), bestRssi);
  WiFi.begin(creds[bestCred].ssid.c_str(), creds[bestCred].pass.c_str());

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < kWiFiTimeoutMs) {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] OK: %s (%s)\n", creds[bestCred].ssid.c_str(), WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("[WiFi] connect failed");
  return false;
}

// ----------------------------------------------------
// WiFi event handlers
// ----------------------------------------------------
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  (void)info;
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[WiFi] event: STA disconnected");
      s_wifiOk         = false;
      s_needResumePlay = s_playing;
      s_cmdStop        = true; // Safely tell the task to stop audio
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.println("[WiFi] event: GOT_IP");
      s_wifiOk = true;
      if (s_needResumePlay) {
        Serial.println("[NET] resume after wifi");
      }
      s_cmdPlay = true; // Safely tell the task to resume audio
      s_needResumePlay = false;
      break;

    default:
      break;
  }
}

// ----------------------------------------------------
// Public API
// ----------------------------------------------------

/**
 * @brief Starts the Network Radio mode.
 * @note Connects to Wi-Fi, loads stations, and begins streaming audio.
 */
void modeNetStart(){
  loadStations("/system/netradio/stations.txt");

  // 1) Initialize audio and start the wait tone immediately
  if (!audioStartNetMode()) {
    Serial.println("[NET] could not acquire I2S");
  }
  
  audioReleaseNet();    // Give up I2S ownership to LOCAL for wait tone
  audioPlay(kWaitTonePath);
  s_playingWaitTone = true;

  // Initialize tuner variables before starting the task
  int bin = adcTunerBin16();
  s_lastBin     = bin;
  s_pendingBin  = bin;
  s_lastTuneMs  = millis();
  s_ignoreTunerUntil = millis() + kIgnoreTunerWifiMs; // Safely ignore the tuner while Wi-Fi connects

  // Start the background task early so the wait tone loops during Wi-Fi setup
  if (s_task) {
    s_killTask = true;
    while(s_task != nullptr) { delay(10); } // Wait for task to safely exit
  }
  s_killTask = false;
  xTaskCreatePinnedToCore(netTask, "netTask", 4096, nullptr, 3, &s_task, 1);

  // 2) Setup Wi-Fi and connect (this blocks for a few seconds)
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);

  bool wifiUp = connectWiFiFromFile();
  s_wifiOk    = wifiUp;

  // Refresh tuner position now that Wi-Fi connection has concluded
  bin = adcTunerBin16();
  s_lastBin     = bin;
  s_pendingBin  = bin;
  s_lastTuneMs  = millis();
  s_ignoreTunerUntil = millis() + kIgnoreTunerBootMs; // ignore initial noise

  Serial.printf("[NET] init bin=%d\n", bin);
  if (bin == 0 || (bin - 1) >= s_stCount) {
    // Invalid/empty bin; simply keep the wait tone playing
    Serial.println("[NET] WAIT: empty bin or 0");
  } else {
    s_wantedIndex = bin - 1;
    if (s_wifiOk) {
      s_cmdPlay = true; // Defer to the task to prevent threading collisions
    } else {
      Serial.println("[NET] wifi not ready - will play when connected");
      s_needResumePlay = true;
    }
  }

  Serial.println("Booted into mode 2");
}

/**
 * @brief Stops the Network mode and cleanly shuts down active streams and Wi-Fi.
 */
void modeNetStop(){
  if (s_task) {
    s_killTask = true;
    while(s_task != nullptr) { delay(10); } // Wait for task to safely exit
  }
  if (s_playingWaitTone) {
    audioReleaseLocal();
    s_playingWaitTone = false;
  } else {
    stopAudio();
    audioReleaseNet();
  }
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  Serial.println("[NET] stop");
}

// ----------------------------------------------------
// FreeRTOS Task
// ----------------------------------------------------
static void netTask(void*) {
  const TickType_t tick = pdMS_TO_TICKS(kTaskTickMs); // Reduced to prevent audio stuttering

  for (;;) {
    if (s_killTask) {
      s_task = nullptr;
      vTaskDelete(NULL); // Safely deletes itself
    }

    uint32_t now = millis();

    // Handle Thread-Safe Commands from other tasks
    if (s_cmdStop) {
      s_cmdStop = false;
      stopAudio();
      s_needStreamRetry = false;
    }
    if (s_cmdPlay) {
      s_cmdPlay = false;
      if (s_wantedIndex >= 0 && s_wantedIndex < s_stCount) {
        applyStation(s_wantedIndex);
      }
    }

    // Keep the wait tone playing seamlessly
    if (s_playingWaitTone && !audioIsPlaying()) {
      audioPlay(kWaitTonePath);
    }

    // 1) Audio stream copying - Must run frequently!
    s_inNetCopy = true;
    bool do_copy = !s_playingWaitTone && s_playing && s_copy;
    size_t n = 0;
    if (do_copy) {
      n = s_copy->copy();
    }
    s_inNetCopy = false;

    if (do_copy && n > 0) {
      s_lastAudioData = now;
      s_rxBytesSec   += n;
    }

    // 2) Tuner polling
    if (now >= s_ignoreTunerUntil) {
      int8_t bin = adcTunerBin16();
      
      // Register change
      if (bin != s_pendingBin) {
        s_pendingBin = bin;
        s_lastTuneMs = now;
        playWaitTone();
      }

      // Confirm after ~1.5s
      if ((now - s_lastTuneMs) > kTuneConfirmMs) {
        if (s_pendingBin == 0) {
          if (s_lastBin != 0) {
            s_lastBin = 0;
            Serial.println("[NET] WAIT: out of range");
          }
        } else if (s_pendingBin != s_lastBin) {
          s_lastBin = s_pendingBin;
          int stIdx = s_lastBin - 1;
          Serial.printf("[NET] Confirmed bin %d\n", s_lastBin);
          if (stIdx >= 0 && stIdx < s_stCount) {
            s_wantedIndex = stIdx;
            if (s_wifiOk) applyStation(stIdx);
            else          s_needResumePlay = true;
          } else {
            Serial.println("[NET] WAIT: empty bin"); // Valid bin, but no station configured
          }
        }
      }
    }

  // Debug print every second
  now = millis(); // Refresh 'now' because applyStation() takes several seconds and causes underflows
  if (now - s_lastRxPrint > kRxPrintIntervalMs) {
    s_lastRxPrint = now;
    if (!s_playingWaitTone && s_playing)
      Serial.printf("[NET] rx=%u B/s\n", (unsigned)s_rxBytesSec);
    s_rxBytesSec = 0;
  }

  // stream watchdog
  if (!s_playingWaitTone && s_playing && (now - s_lastAudioData > kStreamStallMs)) {
    Serial.println("[NET] stream stalled -> will retry");
    stopAudio();
    s_needStreamRetry   = true;
    s_lastStreamAttempt = now;
  }

  // retry
  if (!s_playingWaitTone &&
      s_wifiOk &&
      !s_playing &&
      s_needStreamRetry &&
      (now - s_lastStreamAttempt > kStreamRetryMs) &&
      s_index >= 0 && s_index < s_stCount) {
    Serial.println("[NET] retrying stalled stream...");
    applyStation(s_index);
  }

    vTaskDelay(tick);
  }
}

/**
 * @brief Reloads the current station stream (e.g., if the connection dropped).
 */
void modeNetReload(){
  if (s_playingWaitTone) return; // Ignore reload clicks while tuning
  if (s_wantedIndex>=0 && s_wantedIndex<s_stCount) {
    if (s_wifiOk) s_cmdPlay = true;
    else          s_needResumePlay = true;
  }
}

/**
 * @brief Initiates Wi-Fi Protected Setup (WPS) connection.
 */
void modeNetPlayPauseToggle(){
  if (s_playingWaitTone) return; // Ignore play/pause clicks while tuning
  
  if (s_playing) {
    s_cmdStop = true;
  } else {
    if (s_wantedIndex>=0 && s_wantedIndex<s_stCount) {
      if (s_wifiOk) s_cmdPlay = true;
      else          s_needResumePlay = true;
    }
  }
}

/**
 * @brief Initiates Wi-Fi Protected Setup (WPS) connection.
 */
void WPS(){
  // Empty implementation
}
