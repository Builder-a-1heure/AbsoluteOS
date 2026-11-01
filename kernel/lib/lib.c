// ============================================================================
// lib.c — Mini bibliothèque C. Sans libc, GCC a quand même besoin de ces 4
// fonctions (il les appelle lui-même pour copier des structures, etc.).
// ============================================================================
#include <stddef.h>
#include <stdint.h>

// Remplit n octets de s avec la valeur c
void *memset(void *s, int c, size_t n) {
    uint8_t *p = s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

// Copie n octets de src vers dst (zones qui ne se chevauchent pas)
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

// Comme memcpy, mais sûre si les zones se chevauchent (copie à l'envers si besoin)
void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    if (d < s) { while (n--) *d++ = *s++; }
    else       { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

// Compare n octets : 0 si égaux, sinon différence du premier octet distinct
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}

// Longueur d'une chaîne terminée par 0
size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

// Compare deux chaînes : 0 si égales, sinon différence du premier octet distinct
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}
