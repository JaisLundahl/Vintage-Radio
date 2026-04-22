// RebootService.h
#pragma once
#include "InputService.h"  // for AppMode

// Kontrolleret reboot fra en given aktiv mode
void rebootFromMode(AppMode currentMode);
void safeShutdown(AppMode currentMode);