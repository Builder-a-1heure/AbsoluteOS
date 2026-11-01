// ============================================================================
// kprintf.c — printf minimal du noyau
// ============================================================================
#include <stdarg.h>
#include <stdint.h>
#include "kprintf.h"
#include "uart.h"
#include "console.h"

// Sortie unique : tout caractère part vers le port série et vers l'écran
void kputc(char c) {
    uart_putc(c);
    console_putc(c);
}

void kputs(const char *s) { while (*s) kputc(*s++); }

// Affiche un nombre dans une base donnée, avec largeur minimale et remplissage ('0' ou ' ')
static void put_number(uint64_t v, unsigned base, int upper, int negative, int width, char pad) {
    char buf[24]; int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { buf[n++] = digits[v % base]; v /= base; } while (v);   // chiffres à l'envers
    int len = n + (negative ? 1 : 0);
    if (pad == '0' && negative) kputc('-');                     // signe avant les zéros
    for (; len < width; len++) kputc(pad);
    if (pad != '0' && negative) kputc('-');                     // signe après les espaces
    while (n) kputc(buf[--n]);
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { kputc(*fmt); continue; }
        fmt++;
        char pad = ' '; int width = 0, longs = 0, left = 0;
        if (*fmt == '-') { left = 1; fmt++; }                   // drapeau - : aligné à gauche (%s)
        if (*fmt == '0') { pad = '0'; fmt++; }                  // drapeau 0
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') { longs++; fmt++; }                 // l / ll : 64 bits
        switch (*fmt) {
        case 'c': kputc((char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *); if (!s) s = "(null)";
            int len = 0; while (s[len]) len++;
            if (!left) for (; len < width; len++) kputc(' ');            // alignement à droite
            kputs(s);
            if (left) for (; len < width; len++) kputc(' ');             // alignement à gauche
            break; }
        case 'd': case 'i': {
            int64_t v = longs ? va_arg(ap, int64_t) : va_arg(ap, int);
            put_number(v < 0 ? -(uint64_t)v : (uint64_t)v, 10, 0, v < 0, width, pad); break; }
        case 'u': put_number(longs ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, 0, 0, width, pad); break;
        case 'x': put_number(longs ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, 0, 0, width, pad); break;
        case 'X': put_number(longs ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, 1, 0, width, pad); break;
        case 'p': kputs("0x"); put_number((uint64_t)va_arg(ap, void *), 16, 0, 0, 16, '0'); break;
        case '%': kputc('%'); break;
        case 0:   va_end(ap); return;                           // format terminé par un '%'
        default:  kputc('%'); kputc(*fmt); break;               // spécificateur inconnu : tel quel
        }
    }
    va_end(ap);
}
