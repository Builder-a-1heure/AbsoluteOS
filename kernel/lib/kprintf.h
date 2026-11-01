// ============================================================================
// kprintf.h — Affichage formaté, envoyé à l'écran ET au port série
// Formats : %c %s %d %i %u %x %X %p %% ; modificateurs : l / ll, largeur, 0
// ============================================================================
#pragma once
void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...);
