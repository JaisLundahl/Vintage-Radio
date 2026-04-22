#include "output_service.h"
#include "pins.h"
#include <driver/ledc.h>

// Singleton instance
static OutputService g_outputService;
OutputService& outputService() { return g_outputService; }

// LEDC channels
static const int CH_R = 0;
static const int CH_G = 1;
static const int CH_B = 2;

void OutputService::begin() {
    // Arduino-style init
    ledcSetup(CH_R, 5000, 8);
    ledcSetup(CH_G, 5000, 8);
    ledcSetup(CH_B, 5000, 8);

    ledcAttachPin(PIN_LED_R, CH_R);
    ledcAttachPin(PIN_LED_G, CH_G);
    ledcAttachPin(PIN_LED_B, CH_B);

    // Enable ESP-IDF fade driver
    ledc_fade_func_install(0);

    setRGB(0, 0, 0);
}

void OutputService::setState(SystemState s) {
    current = s;

    // Apply a default LED color when state changes
    switch (current) {
        case SystemState::INIT:
            // Pulse blue while booting
            pulseBlue4s();
            break;
        case SystemState::READY:
            // Solid green
            setRGB(0, 255, 0);
            // Stop all pulses
            pulse[0].active = pulse[1].active = pulse[2].active = false;
            break;
        case SystemState::PAIR_MODE:
            // Red pulse
            pulseRed4s();
            break;
        default:
            break;
    }
}

void OutputService::loop() {
    // Service potential pulses
    servicePulseChannel(CH_R);
    servicePulseChannel(CH_G);
    servicePulseChannel(CH_B);

    // Additional blink logic can go here
}

void OutputService::setRGB(uint8_t r, uint8_t g, uint8_t b) {
    ledcWrite(CH_R, r);
    ledcWrite(CH_G, g);
    ledcWrite(CH_B, b);
}

// --------- Pulsing ---------

void OutputService::startPulseOnChannel(uint8_t ch) {
    // Disable other pulses - we only show one color at a time
    for (int i = 0; i < 3; ++i) {
        pulse[i].active = false;
    }

    pulse[ch].active = true;
    pulse[ch].rising = true;
    pulse[ch].nextSwitchMs = millis() + PULSE_HALF_MS;

    // Start: 0 -> 255 over 2 seconds
    ledc_set_fade_time_and_start(
        LEDC_HIGH_SPEED_MODE,
        (ledc_channel_t)ch,
        255,
        PULSE_HALF_MS,
        LEDC_FADE_NO_WAIT
    );
}

void OutputService::servicePulseChannel(uint8_t ch) {
    if (!pulse[ch].active) return;

    uint32_t now = millis();
    if ((int32_t)(now - pulse[ch].nextSwitchMs) >= 0) {
        // Reverse direction
        pulse[ch].rising = !pulse[ch].rising;
        pulse[ch].nextSwitchMs = now + PULSE_HALF_MS;

        uint32_t target = pulse[ch].rising ? 255 : 0;

        ledc_set_fade_time_and_start(
            LEDC_HIGH_SPEED_MODE,
            (ledc_channel_t)ch,
            target,
            PULSE_HALF_MS,
            LEDC_FADE_NO_WAIT
        );
    }
}

void OutputService::pulseRed4s() {
    startPulseOnChannel(CH_R);
}

void OutputService::pulseGreen4s() {
    startPulseOnChannel(CH_G);
}

void OutputService::pulseBlue4s() {
    startPulseOnChannel(CH_B);
}
