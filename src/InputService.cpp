#include "InputService.h"
#include "AdcService.h"   // for adcModeSelRaw12b()

// --- tunables ---
static const uint16_t DEBOUNCE_MS      = 20;
static const uint16_t LONGPRESS_MS     = 800;
static const uint16_t VERY_LONGPRESS_MS= 5000;
static const uint16_t MODE_DEBOUNCE_MS = 200;

// --- state ---
static bool s_inited = false;

struct BtnState {
  uint8_t        pin;
  InputEventType evShort;   // event fired on short release
  InputEventType evLong;    // event fired on long hold (EV_NONE = disabled)
  InputEventType evVeryLong;// event fired on 5 sec hold
  bool           stable;
  bool           reading;
  uint32_t       tChange;
  bool           pressed;
  uint32_t       tPressed;
  bool           firedLong;
  bool           firedVeryLong;
};

// encoder button: short press → EV_SHORT_PRESS, long press → EV_LONG_PRESS, very long press → EV_VERY_LONG_PRESS
static BtnState s_encBtn { PIN_ENC_BTN, EV_SHORT_PRESS, EV_LONG_PRESS, EV_VERY_LONG_PRESS, true, true, 0, false, 0, false, false };

#if CURRENT_INPUT_MODE == INPUT_MODE_BUTTONS
// A/B as independent push buttons: A = back, B = forward
static BtnState s_encA   { PIN_ENC_A,   EV_ROT_CCW,    EV_NONE,       EV_NONE,       true, true, 0, false, 0, false, false };
static BtnState s_encB   { PIN_ENC_B,   EV_ROT_CW,     EV_NONE,       EV_NONE,       true, true, 0, false, 0, false, false };
#endif

// mode via ADC selector
static AppMode  s_modeStable  = MODE_IDLE;
static uint32_t s_modeTChange = 0;
static AppMode  s_modeRaw     = MODE_IDLE;

// Small queue
static const uint8_t QSIZE = 8;
static InputEvent s_q[QSIZE];
static uint8_t s_qHead = 0, s_qTail = 0;

static inline void qpush(const InputEvent& e) {
  uint8_t n = (s_qHead + 1) % QSIZE;
  if (n == s_qTail) {
    // Overflow -> discard oldest
    s_qTail = (s_qTail + 1) % QSIZE;
  }
  s_q[s_qHead] = e;
  s_qHead = n;
}
static inline bool qpop(InputEvent& o) {
  if (s_qHead == s_qTail) return false;
  o = s_q[s_qTail];
  s_qTail = (s_qTail + 1) % QSIZE;
  return true;
}

static AppMode decodeModeFromAdc(uint16_t raw) {
  // 0..4095, Vref~3.3V
  // BT~0V, MP3~1.65V, NET~3.3V
  if (raw < 400) return MODE_BT;
  if (raw > 1400 && raw < 2600) return MODE_MP3;
  if (raw > 3000) return MODE_NET;
  return MODE_IDLE;
}

static void updateButton(BtnState& b) {
  bool raw = digitalRead(b.pin);
  uint32_t now = millis();

  if (raw != b.reading) {
    b.reading = raw;
    b.tChange = now;
  }

  if ((now - b.tChange) >= DEBOUNCE_MS && b.stable != b.reading) {
    b.stable = b.reading;

    if (b.stable == LOW) {
      // press
      b.pressed   = true;
      b.tPressed  = now;
      b.firedLong = false;
      b.firedVeryLong = false;
    } else {
      // release
      if (b.pressed) {
        if (b.firedVeryLong) {
          if (b.evVeryLong != EV_NONE) {
            InputEvent ev;
            ev.type = EV_VERY_LONG_RELEASE;
            ev.mode = inputsCurrentMode();
            ev.t_ms = now;
            qpush(ev);
          }
        } else if (!b.firedLong) {
          InputEvent ev;
          ev.type = b.evShort;
          ev.mode = inputsCurrentMode();
          ev.t_ms = now;
          qpush(ev);
        }
      }
      b.pressed   = false;
      b.firedLong = false;
      b.firedVeryLong = false;
    }
  }

  // hold detection
  if (b.pressed) {
    uint32_t holdTime = now - b.tPressed;
    
    if (!b.firedVeryLong && b.evVeryLong != EV_NONE && holdTime >= VERY_LONGPRESS_MS) {
      b.firedVeryLong = true;
      InputEvent ev;
      ev.type = b.evVeryLong;
      ev.mode = inputsCurrentMode();
      ev.t_ms = now;
      qpush(ev);
    } else if (!b.firedLong && b.evLong != EV_NONE && holdTime >= LONGPRESS_MS) {
      b.firedLong = true;
      InputEvent ev;
      ev.type = b.evLong;
      ev.mode = inputsCurrentMode();
      ev.t_ms = now;
      qpush(ev);
    }
  }
}



#if CURRENT_INPUT_MODE == INPUT_MODE_ROTARY
static void updateEncoderRotary() {
  static uint8_t  lastAB = 0;
  static uint32_t lastStepTime = 0;

  uint8_t a  = digitalRead(PIN_ENC_A);
  uint8_t b  = digitalRead(PIN_ENC_B);
  uint8_t ab = (a << 1) | b;
  uint32_t now = millis();

  if (ab != lastAB) {
    uint8_t idx = (lastAB << 2) | ab;
    static const int8_t dirTable[16] = {
      0,  -1,  1,  0,
      1,   0,  0, -1,
     -1,   0,  0,  1,
      0,   1, -1,  0
    };
    int8_t dir = dirTable[idx];
    if (dir != 0 && (now - lastStepTime) >= DEBOUNCE_MS) {
      InputEvent ev;
      ev.type = (dir > 0) ? EV_ROT_CW : EV_ROT_CCW;
      ev.mode = inputsCurrentMode();
      ev.t_ms = now;
      qpush(ev);
      lastStepTime = now;
    }
    lastAB = ab;
  }
}
#endif

static void updateModeSelector() {
  uint32_t now = millis();
  uint16_t raw = adcModeSelRaw12b();
  AppMode  m = decodeModeFromAdc(raw);

  if (m != s_modeRaw) {
    s_modeRaw = m;
    s_modeTChange = now;
  }

  if ((now - s_modeTChange) >= MODE_DEBOUNCE_MS) {
    if (m != s_modeStable) {
      s_modeStable = m;
      InputEvent ev;
      ev.type = EV_MODE_CHANGED;
      ev.mode = m;
      ev.t_ms = now;
      qpush(ev);
    }
  }
}

void inputsInit() {
  // encoder pins
  pinMode(PIN_ENC_A,   INPUT_PULLUP);
  pinMode(PIN_ENC_B,   INPUT_PULLUP);
  pinMode(PIN_ENC_BTN, INPUT_PULLUP);

  // init btn state
  s_encBtn.stable  = s_encBtn.reading = digitalRead(PIN_ENC_BTN);
  s_encBtn.tChange = millis();

#if CURRENT_INPUT_MODE == INPUT_MODE_BUTTONS
  // init A/B as independent push buttons
  s_encA.stable = s_encA.reading = digitalRead(PIN_ENC_A);
  s_encA.tChange = millis();
  s_encB.stable = s_encB.reading = digitalRead(PIN_ENC_B);
  s_encB.tChange = millis();
#else
  // init quadrature state
  { uint8_t a = digitalRead(PIN_ENC_A); uint8_t b = digitalRead(PIN_ENC_B); (void)a; (void)b; }
#endif

  // init mode state from ADC selector
  uint16_t raw = adcModeSelRaw12b();
  s_modeRaw = decodeModeFromAdc(raw);
  s_modeStable  = s_modeRaw;
  s_modeTChange = millis();

  s_inited = true;

  // If your original file uses a task, keep it - just showing the simple approach here
}

void inputsTick() {
  if (!s_inited) return;
  updateButton(s_encBtn);
#if CURRENT_INPUT_MODE == INPUT_MODE_BUTTONS
  updateButton(s_encA);   // A = back
  updateButton(s_encB);   // B = forward
#else
  updateEncoderRotary();
#endif
  updateModeSelector();
}

bool inputsGetEvent(InputEvent& out) {
  return qpop(out);
}

AppMode inputsCurrentMode() {
  return s_modeStable;
}
