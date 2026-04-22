#pragma once
#include <stdint.h>
#include "pins.h"
#include <driver/adc.h>  // hvis ikke allerede

void     adcInit();                 // kald i setup()
void     adcTick();                 // kald i hvert loop()
uint8_t  adcVolumePct();            // 0..100 (filtreret)
uint16_t adcRawVolume();            // 0..4095 (rå, multisamplet)
uint16_t adcBattery_mV();           // VBAT i mV (kræver korrekt divider)
int8_t   adcTunerBin16();           // 0..15 “bin” af tuner-pot
uint16_t adcModeSelRaw12b();        // 0..4095 (glattet ADC mode-selector)
void     adcSetTunerMin();          // Set minimum calibration point
void     adcSetTunerMax();          // Set maximum calibration point
