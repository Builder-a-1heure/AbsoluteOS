// ============================================================================
// keyboard.h — Traduction des touches USB (HID) en caractères
// ============================================================================
#pragma once
#include <stdint.h>

void        kbd_report(const uint8_t *report);   // appelé par le driver USB avec un rapport "boot" de 8 octets
int         kbd_getc_nb(void);                   // octet suivant (UTF-8) ou -1 ; gère aussi la répétition des touches
int         kbd_set_layout(const char *name);    // "fr" ou "us" ; 1 si connu
const char *kbd_layout_name(void);
