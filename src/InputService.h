#pragma once
#include <stdint.h>
#include <Arduino.h>
#include "pins.h"

enum AppMode : uint8_t {
  MODE_BT = 0,
  MODE_MP3,
  MODE_NET,
  MODE_IDLE
};

enum InputEventType : uint8_t {
  EV_NONE = 0,
  EV_SHORT_PRESS,
  EV_LONG_PRESS,
  EV_VERY_LONG_PRESS,
  EV_VERY_LONG_RELEASE,
  EV_ROT_CW,
  EV_ROT_CCW,
  EV_MODE_CHANGED
};

struct InputEvent {
  InputEventType type;
  AppMode        mode;   // mode på det tidspunkt eventet opstod
  uint32_t       t_ms;
};

void     inputsInit();
void     inputsTick();
bool     inputsGetEvent(InputEvent& out);
AppMode  inputsCurrentMode();
