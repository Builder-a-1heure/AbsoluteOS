// ============================================================================
// keyboard.c — Disposition du clavier, majuscules, AltGr, répétition
//
// Un clavier USB n'envoie pas des caractères mais des POSITIONS de touches
// (codes "usage" HID, nommés d'après un clavier US : 0x04 = la touche 'A' US).
// Chaque disposition associe donc un code de position à ce que la touche écrit :
//   normal, avec Maj, avec AltGr.
// Les textes sont en UTF-8 (la console et le shell le comprennent).
//
// Rapport "boot" de 8 octets : [0] modificateurs, [1] réservé, [2..7] touches enfoncées.
// ============================================================================
#include <stdint.h>
#include "keyboard.h"
#include "timer.h"

typedef struct { const char *n, *s, *a; } Key;      // normal, Maj, AltGr (NULL = rien)

#define NKEYS 0x65

// ---- Touches communes aux deux dispositions (Entrée, Tab, pavé numérique...) ----
#define COMMON \
    [0x28] = {"\r", "\r", 0}, [0x2A] = {"\x7f", "\x7f", 0}, [0x2B] = {"\t", "\t", 0}, [0x2C] = {" ", " ", 0}, \
    [0x54] = {"/", "/", 0}, [0x55] = {"*", "*", 0}, [0x56] = {"-", "-", 0}, [0x57] = {"+", "+", 0}, \
    [0x58] = {"\r", "\r", 0}, [0x59] = {"1", "1", 0}, [0x5A] = {"2", "2", 0}, [0x5B] = {"3", "3", 0}, \
    [0x5C] = {"4", "4", 0}, [0x5D] = {"5", "5", 0}, [0x5E] = {"6", "6", 0}, [0x5F] = {"7", "7", 0}, \
    [0x60] = {"8", "8", 0}, [0x61] = {"9", "9", 0}, [0x62] = {"0", "0", 0}, [0x63] = {".", ".", 0}

// ---- AZERTY français (clavier PC/Mac ISO) ----
static const Key layout_fr[NKEYS] = {
    COMMON,
    [0x04] = {"q","Q",0}, [0x05] = {"b","B",0}, [0x06] = {"c","C",0}, [0x07] = {"d","D",0},
    [0x08] = {"e","E","\xe2\x82\xac"},                                   // AltGr+E = €
    [0x09] = {"f","F",0}, [0x0A] = {"g","G",0}, [0x0B] = {"h","H",0}, [0x0C] = {"i","I",0},
    [0x0D] = {"j","J",0}, [0x0E] = {"k","K",0}, [0x0F] = {"l","L",0},
    [0x10] = {",","?",0},                                                // position du M US
    [0x11] = {"n","N",0}, [0x12] = {"o","O",0}, [0x13] = {"p","P",0},
    [0x14] = {"a","A",0},                                                // position du Q US
    [0x15] = {"r","R",0}, [0x16] = {"s","S",0}, [0x17] = {"t","T",0}, [0x18] = {"u","U",0},
    [0x19] = {"v","V",0},
    [0x1A] = {"z","Z",0},                                                // position du W US
    [0x1B] = {"x","X",0}, [0x1C] = {"y","Y",0},
    [0x1D] = {"w","W",0},                                                // position du Z US
    [0x1E] = {"&","1",0},
    [0x1F] = {"\xc3\xa9","2","~"},   [0x20] = {"\"","3","#"},  [0x21] = {"'","4","{"},
    [0x22] = {"(","5","["},          [0x23] = {"-","6","|"},
    [0x24] = {"\xc3\xa8","7","`"},   [0x25] = {"_","8","\\"},
    [0x26] = {"\xc3\xa7","9","^"},   [0x27] = {"\xc3\xa0","0","@"},
    [0x2D] = {")","\xc2\xb0","]"},   [0x2E] = {"=","+","}"},
    [0x2F] = {"^","\"",0},                                               // touche morte simplifiée : écrit ^ directement
    [0x30] = {"$","\xc2\xa3",0},
    [0x31] = {"*","\xc2\xb5",0},     [0x32] = {"*","\xc2\xb5",0},       // touche à gauche d'Entrée : * et µ
    [0x33] = {"m","M",0},            [0x34] = {"\xc3\xb9","%",0},
    [0x35] = {"\xc2\xb2","\xc2\xb2",0},
    [0x36] = {";",".",0},            [0x37] = {":","/",0},              [0x38] = {"!","\xc2\xa7",0},
    [0x64] = {"<",">",0},
};

// ---- QWERTY américain ----
static const Key layout_us[NKEYS] = {
    COMMON,
    [0x04] = {"a","A",0}, [0x05] = {"b","B",0}, [0x06] = {"c","C",0}, [0x07] = {"d","D",0}, [0x08] = {"e","E",0},
    [0x09] = {"f","F",0}, [0x0A] = {"g","G",0}, [0x0B] = {"h","H",0}, [0x0C] = {"i","I",0}, [0x0D] = {"j","J",0},
    [0x0E] = {"k","K",0}, [0x0F] = {"l","L",0}, [0x10] = {"m","M",0}, [0x11] = {"n","N",0}, [0x12] = {"o","O",0},
    [0x13] = {"p","P",0}, [0x14] = {"q","Q",0}, [0x15] = {"r","R",0}, [0x16] = {"s","S",0}, [0x17] = {"t","T",0},
    [0x18] = {"u","U",0}, [0x19] = {"v","V",0}, [0x1A] = {"w","W",0}, [0x1B] = {"x","X",0}, [0x1C] = {"y","Y",0},
    [0x1D] = {"z","Z",0},
    [0x1E] = {"1","!",0}, [0x1F] = {"2","@",0}, [0x20] = {"3","#",0}, [0x21] = {"4","$",0}, [0x22] = {"5","%",0},
    [0x23] = {"6","^",0}, [0x24] = {"7","&",0}, [0x25] = {"8","*",0}, [0x26] = {"9","(",0}, [0x27] = {"0",")",0},
    [0x2D] = {"-","_",0}, [0x2E] = {"=","+",0}, [0x2F] = {"[","{",0}, [0x30] = {"]","}",0},
    [0x31] = {"\\","|",0}, [0x32] = {"#","~",0}, [0x33] = {";",":",0}, [0x34] = {"'","\"",0},
    [0x35] = {"`","~",0}, [0x36] = {",","<",0}, [0x37] = {".",">",0}, [0x38] = {"/","?",0},
    [0x64] = {"\\","|",0},
};

static const Key *g_layout = layout_fr;     // disposition par défaut : AZERTY (changeable avec la commande keymap)
static int g_caps;                          // Verr. Maj actif

// ---- File d'octets en attente de lecture ----
#define FIFO_N 128
static uint8_t g_fifo[FIFO_N];
static uint32_t g_head, g_tail;

static void push_str(const char *s) {
    while (*s) {
        uint32_t next = (g_head + 1) % FIFO_N;
        if (next == g_tail) return;                     // pleine : on jette
        g_fifo[g_head] = (uint8_t)*s++;
        g_head = next;
    }
}

// ---- Répétition automatique (le clavier USB n'en fait pas) ----
static const char *g_held;                  // texte de la touche maintenue (NULL = aucune)
static uint8_t     g_held_usage;
static uint64_t    g_rep_at;

// Texte produit par une touche avec les modificateurs donnés (NULL si rien)
static const char *key_text(uint8_t usage, uint8_t mods) {
    if (usage >= NKEYS) return 0;
    const Key *k = &g_layout[usage];
    if (!k->n) return 0;
    int shift = (mods & 0x22) != 0;                     // Maj gauche (bit 1) ou droite (bit 5)
    int altgr = (mods & 0x44) != 0;                     // Alt gauche (bit 2) ou AltGr (bit 6)
    if (altgr && k->a) return k->a;
    // Verr. Maj n'inverse que les lettres simples a-z
    if (g_caps && k->n[0] >= 'a' && k->n[0] <= 'z' && !k->n[1]) shift = !shift;
    return shift && k->s ? k->s : k->n;
}

// Nouveau rapport : on compare avec le précédent pour trouver les touches nouvellement enfoncées
void kbd_report(const uint8_t *r) {
    static uint8_t prev[8];
    uint8_t mods = r[0];
    for (int i = 2; i < 8; i++) {
        uint8_t u = r[i];
        if (u < 4) continue;                            // 0 = vide, 1-3 = erreurs de "rollover"
        int was_down = 0;
        for (int j = 2; j < 8; j++) if (prev[j] == u) was_down = 1;
        if (was_down) continue;
        if (u == 0x39) { g_caps = !g_caps; continue; }  // Verr. Maj
        const char *t = key_text(u, mods);
        if (!t) continue;
        push_str(t);
        g_held = t; g_held_usage = u;                   // début de la répétition
        g_rep_at = timer_ms() + 500;                    // délai avant répétition : 500 ms
    }
    if (g_held) {                                       // la touche maintenue est-elle toujours enfoncée ?
        int still = 0;
        for (int i = 2; i < 8; i++) if (r[i] == g_held_usage) still = 1;
        if (!still) g_held = 0;
    }
    for (int i = 0; i < 8; i++) prev[i] = r[i];
}

int kbd_getc_nb(void) {
    if (g_held && timer_ms() >= g_rep_at) { push_str(g_held); g_rep_at += 35; }   // ~30 caractères/s
    if (g_tail == g_head) return -1;
    int c = g_fifo[g_tail];
    g_tail = (g_tail + 1) % FIFO_N;
    return c;
}

int kbd_set_layout(const char *name) {
    if (name[0] == 'f' && name[1] == 'r' && !name[2]) { g_layout = layout_fr; return 1; }
    if (name[0] == 'u' && name[1] == 's' && !name[2]) { g_layout = layout_us; return 1; }
    return 0;
}

const char *kbd_layout_name(void) { return g_layout == layout_fr ? "fr" : "us"; }
