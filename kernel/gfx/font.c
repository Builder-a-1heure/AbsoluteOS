// ============================================================================
// font.c — Lecture d'un fichier PSF déjà présent en mémoire
// ============================================================================
#include "font.h"
#include "kstring.h"

// Table "code Unicode -> numéro de glyphe" construite au chargement (0xFFFF = absent)
static uint16_t g_unimap[0x10000];

// Lit un entier 32 bits little-endian octet par octet (pas d'accès non aligné)
static uint32_t rd32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Lit la table Unicode d'un PSF1 : pour chaque glyphe, des codes 16 bits terminés par 0xFFFF
// (0xFFFE introduit une séquence de caractères combinés, ignorée ici)
static void map_psf1(const uint8_t *t, const uint8_t *end, uint32_t count) {
    for (uint32_t g = 0; g < count && t + 2 <= end; g++) {
        int seq = 0;
        for (;;) {
            if (t + 2 > end) return;
            uint32_t v = t[0] | (t[1] << 8); t += 2;
            if (v == 0xFFFF) break;                         // fin de la liste de ce glyphe
            if (v == 0xFFFE) { seq = 1; continue; }         // la suite est une séquence : on l'ignore
            if (!seq && g_unimap[v] == 0xFFFF) g_unimap[v] = (uint16_t)g;
        }
    }
}

// Lit la table Unicode d'un PSF2 : pour chaque glyphe, des caractères UTF-8 terminés par 0xFF
// (0xFE introduit une séquence, ignorée ici)
static void map_psf2(const uint8_t *t, const uint8_t *end, uint32_t count) {
    for (uint32_t g = 0; g < count && t < end; g++) {
        int seq = 0;
        while (t < end && *t != 0xFF) {
            if (*t == 0xFE) { seq = 1; t++; continue; }
            uint32_t cp, n;
            if      (*t < 0x80) { cp = *t;        n = 1; }
            else if (*t < 0xE0) { cp = *t & 0x1F; n = 2; }
            else if (*t < 0xF0) { cp = *t & 0x0F; n = 3; }
            else                { cp = *t & 0x07; n = 4; }
            for (uint32_t i = 1; i < n && t + i < end; i++) cp = (cp << 6) | (t[i] & 0x3F);
            t += n;
            if (!seq && cp < 0x10000 && g_unimap[cp] == 0xFFFF) g_unimap[cp] = (uint16_t)g;
        }
        t++;                                                // saute le 0xFF
    }
}

// Numéro de glyphe pour un code Unicode (-1 si la police ne l'a pas)
int font_find(const Font *f, uint32_t cp) {
    if (!f->has_map) return cp < f->count ? (int)cp : -1;   // pas de table : on suppose glyphe = code
    if (cp >= 0x10000 || g_unimap[cp] == 0xFFFF) return -1;
    return g_unimap[cp];
}

// Analyse l'en-tête PSF et remplit la structure Font
int font_load_psf(Font *f, const void *data, size_t size) {
    const uint8_t *d = data;
    if (size < 4) return 0;

    if (d[0] == 0x36 && d[1] == 0x04) {                 // ---- PSF1 : magic 0x0436
        uint8_t mode = d[2];                            // bit 0 : 512 glyphes ; bits 1-2 : table unicode
        f->width  = 8;
        f->height = d[3];                               // octet 3 = taille d'un glyphe = hauteur
        f->count  = (mode & 1) ? 512 : 256;
        f->bytes_per_glyph = d[3];
        f->glyphs = d + 4;                              // l'en-tête PSF1 fait 4 octets
    } else if (size >= 32 && rd32(d) == 0x864ab572) {   // ---- PSF2 : magic 72 b5 4a 86
        uint32_t headersize = rd32(d + 8);
        f->count  = rd32(d + 16);
        f->bytes_per_glyph = rd32(d + 20);
        f->height = rd32(d + 24);
        f->width  = rd32(d + 28);
        f->glyphs = d + headersize;
    } else {
        return 0;                                       // ni PSF1 ni PSF2
    }

    // Vérifie que tous les glyphes tiennent dans le fichier
    if (f->width == 0 || f->height == 0 || f->count == 0) return 0;
    size_t glyph_bytes = (size_t)f->count * f->bytes_per_glyph;
    if ((size_t)(f->glyphs - d) + glyph_bytes > size) return 0;

    // Table Unicode (si présente) : elle suit les glyphes
    memset(g_unimap, 0xFF, sizeof(g_unimap));
    const uint8_t *table = f->glyphs + glyph_bytes, *end = d + size;
    f->has_map = 0;
    if (d[0] == 0x36 && (d[2] & 6) && table < end) { map_psf1(table, end, f->count); f->has_map = 1; }
    else if (d[0] != 0x36 && (rd32(d + 12) & 1) && table < end) { map_psf2(table, end, f->count); f->has_map = 1; }
    return 1;
}

// Adresse du glyphe n ; si n est hors police, on prend '?'
const uint8_t *font_glyph(const Font *f, uint32_t index) {
    if (index >= f->count) index = '?';
    return f->glyphs + (size_t)index * f->bytes_per_glyph;
}

// Pixel (x,y) d'un glyphe : 1 s'il est allumé
int font_pixel(const Font *f, const uint8_t *glyph, uint32_t x, uint32_t y) {
    uint32_t stride = (f->width + 7) / 8;               // octets par ligne de glyphe
    return (glyph[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1;
}
