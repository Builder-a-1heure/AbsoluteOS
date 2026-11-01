// ============================================================================
// console.c — Console texte sur le framebuffer
//
// L'écran est vu comme une grille cols x rows de cellules (caractère + couleurs).
// La grille est conservée en mémoire (g_chr / g_att) : ça permet de redessiner
// une cellule seule (curseur) et de défiler sans relire l'écran.
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include "console.h"
#include "font.h"

extern const uint8_t font_psf_start[], font_psf_end[];   // fichier .psf embarqué (font_data.S)

#define MAX_COLS 512
#define MAX_ROWS 256

static Font      g_font;
static int       g_ready;
static volatile uint32_t *g_fb;          // framebuffer, vu comme un tableau de pixels 32 bits
static uint32_t  g_pitch_words;          // pixels (32 bits) par ligne d'écran
static uint32_t  g_fmt;                  // 0 = octets R,G,B,x ; 1 = octets B,G,R,x
static uint32_t  g_fw, g_fh;             // taille d'une cellule en pixels
static uint32_t  g_cols, g_rows;         // taille de la grille
static uint32_t  g_col, g_row;           // position du curseur
static uint8_t   g_fg = CON_LGRAY, g_bg = CON_BLACK;   // couleurs courantes
static int       g_cursor_on;            // 1 si le curseur est actuellement dessiné
static int       g_bold, g_inverse;      // attributs ANSI : gras (couleurs vives), vidéo inversée
#define DEF_FG CON_LGRAY                 // couleurs par défaut (séquences ANSI 0, 39, 49)
#define DEF_BG CON_BLACK
static uint16_t  g_chr[MAX_COLS * MAX_ROWS];  // glyphe de chaque cellule (les polices peuvent avoir 512 glyphes)
static uint8_t   g_att[MAX_COLS * MAX_ROWS];  // couleurs : (fg << 4) | bg

// Palette RGB (0xRRGGBB)
static const uint32_t palette[16] = {
    0x000000, 0xaa0000, 0x00aa00, 0xaa5500, 0x0000aa, 0xaa00aa, 0x00aaaa, 0xaaaaaa,
    0x555555, 0xff5555, 0x55ff55, 0xffff55, 0x5555ff, 0xff55ff, 0x55ffff, 0xffffff,
};

// Convertit un indice de palette en valeur de pixel selon l'ordre des octets du GOP
static uint32_t px(uint8_t idx) {
    uint32_t c = palette[idx & 15];
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return g_fmt == 0 ? (r | g << 8 | b << 16) : (b | g << 8 | r << 16);
}

// Dessine la cellule (r,c) ; inverse = 1 échange fond et texte (sert au curseur)
static void draw_cell(uint32_t r, uint32_t c, int inverse) {
    uint32_t i = r * g_cols + c;
    uint8_t att = g_att[i];
    uint32_t fg = px(inverse ? (att & 15) : (att >> 4));
    uint32_t bg = px(inverse ? (att >> 4) : (att & 15));
    const uint8_t *gl = font_glyph(&g_font, g_chr[i]);
    for (uint32_t y = 0; y < g_fh; y++) {
        volatile uint32_t *line = g_fb + (size_t)(r * g_fh + y) * g_pitch_words + c * g_fw;
        for (uint32_t x = 0; x < g_fw; x++)
            line[x] = font_pixel(&g_font, gl, x, y) ? fg : bg;
    }
}

// Remplit un rectangle de pixels d'une couleur unie
static void fill_px(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t j = 0; j < h; j++) {
        volatile uint32_t *line = g_fb + (size_t)(y + j) * g_pitch_words + x;
        for (uint32_t i = 0; i < w; i++) line[i] = color;
    }
}

static void cursor_hide(void) {
    if (g_cursor_on && g_col < g_cols) draw_cell(g_row, g_col, 0);
    g_cursor_on = 0;
}

void console_cursor_show(void) {
    if (!g_ready || g_cursor_on) return;
    if (g_col < g_cols) draw_cell(g_row, g_col, 1);
    g_cursor_on = 1;
}

// Efface tout l'écran et remet le curseur en haut à gauche
void console_clear(void) {
    if (!g_ready) return;
    for (uint32_t i = 0; i < g_cols * g_rows; i++) { g_chr[i] = ' '; g_att[i] = (g_fg << 4) | g_bg; }
    fill_px(0, 0, g_cols * g_fw, g_rows * g_fh, px(g_bg));
    g_col = g_row = 0;
    g_cursor_on = 0;
}

// Remonte tout le contenu d'une ligne de texte et vide la dernière
static void scroll_up(void) {
    uint32_t n = g_cols * g_rows;
    for (uint32_t i = 0; i + g_cols < n; i++) { g_chr[i] = g_chr[i + g_cols]; g_att[i] = g_att[i + g_cols]; }
    for (uint32_t i = n - g_cols; i < n; i++) { g_chr[i] = ' '; g_att[i] = (g_fg << 4) | g_bg; }
    uint32_t words = g_cols * g_fw;                       // pixels utiles par ligne d'écran
    for (uint32_t y = 0; y < (g_rows - 1) * g_fh; y++) {  // copie les lignes de pixels vers le haut
        volatile uint32_t *dst = g_fb + (size_t)y * g_pitch_words;
        volatile uint32_t *src = g_fb + (size_t)(y + g_fh) * g_pitch_words;
        for (uint32_t x = 0; x < words; x++) dst[x] = src[x];
    }
    fill_px(0, (g_rows - 1) * g_fh, words, g_fh, px(g_bg));
}

static void newline(void) {
    g_col = 0;
    if (++g_row >= g_rows) { scroll_up(); g_row = g_rows - 1; }
}

// Place un glyphe à la position du curseur et avance
static void put_glyph(uint16_t glyph) {
    if (g_col >= g_cols) newline();                       // retour à la ligne automatique
    uint32_t i = g_row * g_cols + g_col;
    g_chr[i] = glyph;
    g_att[i] = g_inverse ? ((g_bg << 4) | g_fg) : ((g_fg << 4) | g_bg);   // vidéo inversée : on échange
    draw_cell(g_row, g_col, 0);
    g_col++;
}

// Code Unicode -> numéro de glyphe, d'après la table Unicode de la police ('?' si absent)
static uint16_t map_codepoint(uint32_t cp) {
    int g = font_find(&g_font, cp);
    if (g < 0) g = font_find(&g_font, '?');
    return g < 0 ? '?' : (uint16_t)g;
}

// Applique un code ANSI SGR (couleurs et attributs), ex. 0 = reset, 93 = jaune vif, 41 = fond rouge
static void apply_sgr(int p) {
    if (p == 0)       { g_fg = DEF_FG; g_bg = DEF_BG; g_bold = 0; g_inverse = 0; }
    else if (p == 1)  { g_bold = 1; if (g_fg < 8) g_fg += 8; }
    else if (p == 22) { g_bold = 0; }
    else if (p == 7)  { g_inverse = 1; }
    else if (p == 27) { g_inverse = 0; }
    else if (p >= 30 && p <= 37)   g_fg = (p - 30) + (g_bold ? 8 : 0);
    else if (p == 39)              g_fg = DEF_FG;
    else if (p >= 40 && p <= 47)   g_bg = p - 40;
    else if (p == 49)              g_bg = DEF_BG;
    else if (p >= 90 && p <= 97)   g_fg = (p - 90) + 8;      // couleurs vives
    else if (p >= 100 && p <= 107) g_bg = (p - 100) + 8;
}

// Affiche un octet de texte (UTF-8 accepté) : gère \n \r \b \t
void console_putc(char ch) {
    static uint32_t cp, pending;                          // décodage UTF-8 en cours
    static uint8_t  esc;                                  // 0 = texte, 1 = après ESC, 2 = dans ESC [ ... (séquence ANSI)
    static int      prm[8], nprm;                         // paramètres de la séquence ANSI
    uint8_t c = (uint8_t)ch;
    if (!g_ready) return;
    cursor_hide();

    if (esc == 1) {                                       // on attend '[' après ESC
        if (c == '[') { esc = 2; nprm = 0; prm[0] = 0; } else esc = 0;
        return;
    }
    if (esc == 2) {                                       // chiffres ; chiffres ... lettre finale
        if (c >= '0' && c <= '9') { if (nprm < 8) prm[nprm] = prm[nprm] * 10 + (c - '0'); }
        else if (c == ';') { if (nprm < 7) prm[++nprm] = 0; }
        else if (c >= 0x40 && c <= 0x7E) {                // lettre finale : fin de la séquence
            if (c == 'm') for (int i = 0; i <= nprm && i < 8; i++) apply_sgr(prm[i]);   // m = couleurs
            esc = 0;                                      // les autres séquences (curseur...) sont ignorées
        }
        return;
    }
    if (c == 0x1B) { esc = 1; return; }                   // début de séquence ANSI

    if (pending) {                                        // octet de suite d'un caractère UTF-8 ?
        if ((c & 0xC0) == 0x80) {
            cp = (cp << 6) | (c & 0x3F);
            if (--pending == 0) put_glyph(map_codepoint(cp));
            return;
        }
        pending = 0;                                      // séquence cassée : on traite c normalement
    }
    if (c >= 0xC0) {                                      // début de caractère multi-octets
        if (c >= 0xF0)      { pending = 3; cp = c & 0x07; }
        else if (c >= 0xE0) { pending = 2; cp = c & 0x0F; }
        else                { pending = 1; cp = c & 0x1F; }
        return;
    }
    if (c >= 0x80) { put_glyph('?'); return; }            // octet de suite orphelin

    switch (c) {
        case '\n': newline(); break;
        case '\r': g_col = 0; break;
        case '\b': if (g_col > 0) g_col--; break;
        case '\t': g_col = (g_col + 8) & ~7u; if (g_col >= g_cols) newline(); break;
        default:   if (c >= 0x20) put_glyph(c); break;    // caractères imprimables
    }
}

void console_set_color(uint8_t fg, uint8_t bg) { g_fg = fg & 15; g_bg = bg & 15; }
int  console_ready(void)  { return g_ready; }
uint32_t console_cols(void) { return g_cols; }
uint32_t console_rows(void) { return g_rows; }

// Initialise la console : charge la police, calcule la grille, efface l'écran
int console_init(const BootInfo *bi) {
    if (!bi->fb_base || bi->fb_format > 1) return 0;      // pas d'écran exploitable
    if (!font_load_psf(&g_font, font_psf_start, (size_t)(font_psf_end - font_psf_start))) return 0;

    g_fb = (volatile uint32_t *)bi->fb_base;
    g_pitch_words = bi->fb_pitch / 4;
    g_fmt = bi->fb_format;
    g_fw = g_font.width;
    g_fh = g_font.height;
    g_cols = bi->fb_width / g_fw;  if (g_cols > MAX_COLS) g_cols = MAX_COLS;
    g_rows = bi->fb_height / g_fh; if (g_rows > MAX_ROWS) g_rows = MAX_ROWS;
    if (!g_cols || !g_rows) return 0;

    g_ready = 1;
    console_clear();
    return 1;
}
