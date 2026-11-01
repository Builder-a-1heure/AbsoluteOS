// ============================================================================
// timer.h — Horloge monotone (compteur générique arm64)
// ============================================================================
#pragma once
#include <stdint.h>

uint64_t timer_ms(void);       // millisecondes écoulées depuis un point arbitraire (démarrage machine)
