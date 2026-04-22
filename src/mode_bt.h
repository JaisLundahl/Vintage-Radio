#pragma once
#include <Arduino.h>

// kaldes fra main / RebootService
void modeBtStart();
void modeBtStop();
void modeBtLoop();

// knapstyring
void btPlayPauseToggle();
void btNext();
void btPrev();  
void btSetVolumePercent(uint8_t percent);

// manuelt parring
void btEnterPairMode();

// beholdt for kompatibilitet – bruges ikke aktivt mere
bool modeBtCanStop();
