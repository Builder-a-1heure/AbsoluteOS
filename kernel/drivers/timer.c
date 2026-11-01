// ============================================================================
// timer.c — Compteur générique arm64 (CNTVCT_EL0), lisible sans configuration
// Sert aux délais (timeouts USB, répétition des touches) ; pas une heure civile.
// ============================================================================
#include "timer.h"

uint64_t timer_ms(void) {
    uint64_t ticks, freq;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(ticks));   // compteur (isb = lecture à jour)
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));            // fréquence en Hz (réglée par le firmware)
    if (freq < 1000) freq = 1000000;                                 // garde-fou si le firmware ne l'a pas réglée
    return ticks / (freq / 1000);
}
