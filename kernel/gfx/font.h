// ============================================================================
// font.h — Police bitmap au format PSF (PC Screen Font), versions 1 et 2
//   PSF1 : glyphes 8 pixels de large, 256 ou 512 glyphes
//   PSF2 : largeur/hauteur libres
// Dans les deux cas : un glyphe = height lignes, chaque ligne = ceil(width/8) octets,
// bit de poids fort = pixel le plus à gauche.
// ============================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    const uint8_t *glyphs;     // premier glyphe
    uint32_t width, height;    // taille d'un glyphe en pixels
    uint32_t bytes_per_glyph;  // octets par glyphe
    uint32_t count;            // nombre de glyphes
    int      has_map;          // 1 si le fichier contient une table Unicode (code Unicode -> glyphe)
} Font;

int           font_load_psf(Font *f, const void *data, size_t size);  // 1 si OK, 0 si format invalide
int           font_find(const Font *f, uint32_t codepoint);            // numéro de glyphe d'un code Unicode, ou -1
const uint8_t *font_glyph(const Font *f, uint32_t index);               // données du glyphe (ou celui de '?')
int           font_pixel(const Font *f, const uint8_t *glyph, uint32_t x, uint32_t y); // 1 = pixel allumé
