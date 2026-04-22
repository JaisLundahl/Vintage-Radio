#pragma once
#include <Arduino.h>

enum class SystemState {
    INIT,
    READY,
    PAIR_MODE,
    CONNECTING,
    ERROR_STATE,
    NO_KNOWN_PEERS
};

class OutputService {
public:
    void begin();
    void setState(SystemState s);
    void loop();

    // demo: 4s pulsering pr. farve
    void pulseRed4s();
    void pulseGreen4s();
    void pulseBlue4s();

private:
    SystemState current = SystemState::INIT;
    unsigned long lastToggle = 0;
    bool ledOn = false;

    static const uint32_t PULSE_HALF_MS = 2000; // 2s op, 2s ned

    struct PulseState {
        bool active = false;
        bool rising = true;
        uint32_t nextSwitchMs = 0;
    };
    PulseState pulse[3]; // 0=R,1=G,2=B

    void setRGB(uint8_t r, uint8_t g, uint8_t b);
    void startPulseOnChannel(uint8_t ch);
    void servicePulseChannel(uint8_t ch);
};

OutputService& outputService();
