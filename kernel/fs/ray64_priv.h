// ============================================================================
// ray64_priv.h — Accès à la zone SYSTÈME. NE PAS inclure depuis le terminal
// ni depuis les applications : réservé au noyau (chargement de l'OS) et à
// l'installateur système.
// ============================================================================
#pragma once
#include <stdint.h>

uint32_t ray64_sys_blocks(void);                              // taille de la zone système en blocs
int ray64_sys_read(uint32_t index, void *buf);                // lit le bloc n de la zone système
int ray64_sys_write(uint32_t index, const void *buf);         // écrit (ignore la protection) : installateur seulement
