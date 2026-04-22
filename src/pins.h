#pragma once
// ---------------- PINMAP (ESP32-WROOM-32E) ----------------
#ifdef PIN_I2S_BCK
#undef PIN_I2S_BCK
#endif
#ifdef PIN_I2S_LRCK
#undef PIN_I2S_LRCK
#endif
#ifdef PIN_I2S_DATA
#undef PIN_I2S_DATA
#endif
// I2S 
#define PIN_I2S_BCK    14
#define PIN_I2S_LRCK   25
#define PIN_I2S_DATA   27

// I2C for amplifier 
#define PIN_I2C_SDA    21
#define PIN_I2C_SCL    22

// Buttons / selector
// rotary encoder
#define PIN_ENC_A        32    // encoder signal A/back button active low
#define PIN_ENC_B        33    // encoder signal B/forward button active low
#define PIN_ENC_BTN      13  // Encoder push button active low

// Input mode selection
#define INPUT_MODE_ROTARY  0 // Use a standard quadrature rotary encoder
#define INPUT_MODE_BUTTONS 1 // Treat A/B as independent push buttons (back/forward)
#define CURRENT_INPUT_MODE INPUT_MODE_ROTARY // Use a standard quadrature rotary encoder

// Analog mode selector: BT~0V, MP3~1.65V, NET~3.3V
#define PIN_MODE_SEL_ADC 35
//RGB LED pins
#define PIN_LED_R 26
#define PIN_LED_G 2
#define PIN_LED_B 4

// Analog inputs (note: ADC2 cannot be used with WiFi)
#define PIN_VOL_POT      36
#define PIN_TUNER_POT    39
#define PIN_VBAT_ADC     34

// SD card (SPI - using ESP32 standard HSPI)
#define PIN_SD_CS        5
#define PIN_SD_SCK       18
#define PIN_SD_MOSI      23
#define PIN_SD_MISO      19
