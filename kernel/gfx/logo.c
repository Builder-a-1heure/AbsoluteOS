// ============================================================================
// logo.c — Affiche le logo embarqué. Le fichier contient des séquences de
// couleur ANSI : la console les interprète, et un terminal série aussi.
// ============================================================================
#include "logo.h"
#include "kprintf.h"

extern const char logo_ansi_start[], logo_ansi_end[];    // logo_data.S

void logo_show(void) {
    for (const char *p = logo_ansi_start; p < logo_ansi_end; p++) kputc(*p);
}
