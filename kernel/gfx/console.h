// ============================================================================
// console.h — Console texte sur le framebuffer (grille de caractères)
// ============================================================================
#pragma once
#include <stdint.h>
#include "bootinfo.h"

// Palette de 16 couleurs (indices pour console_set_color)
enum {
    CON_BLACK, CON_RED, CON_GREEN, CON_YELLOW, CON_BLUE, CON_MAGENTA, CON_CYAN, CON_LGRAY,
    CON_DGRAY, CON_LRED, CON_LGREEN, CON_LYELLOW, CON_LBLUE, CON_LMAGENTA, CON_LCYAN, CON_WHITE
};

int      console_init(const BootInfo *bi);         // 1 si l'écran est utilisable, 0 sinon (série seulement)
int      console_ready(void);
void     console_putc(char c);                     // gère \n \r \b \t et le texte UTF-8
void     console_clear(void);
void     console_set_color(uint8_t fg, uint8_t bg);
void     console_cursor_show(void);                // affiche le curseur (masqué au prochain caractère)
uint32_t console_cols(void);
uint32_t console_rows(void);
