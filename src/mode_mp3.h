#pragma once
#include <Arduino.h>

// start/stop kaldes af main (og indirekte af RebootService)
void modeMp3Start();
void modeMp3Stop();

// knap-funktioner
void modeMp3Next(); // Plays next song in history, or a new random one
void modeMp3Prev(); // Plays previous song in history
void modeMp3PlayPauseToggle();
