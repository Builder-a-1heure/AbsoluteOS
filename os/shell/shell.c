// ============================================================================
// shell.c — Terminal minimal
//
// POUR AJOUTER UNE COMMANDE : écris une fonction  static void cmd_xxx(const char *args)
// puis ajoute UNE ligne dans le tableau `commands` plus bas.
//
// POUR AJOUTER UNE VALEUR utilisable par `return` (ex: return time) : écris une
// fonction  static void val_xxx(void)  qui affiche la valeur, puis ajoute UNE
// ligne dans le tableau `values`.
//
// Entrée : clavier USB (xHCI) et port série, via input_getc_nb().
// ============================================================================
#include <stdint.h>
#include "shell.h"
#include "shell_fs.h"
#include "kprintf.h"
#include "input.h"
#include "keyboard.h"
#include "console.h"
#include "meminfo.h"
#include "bootinfo.h"

#define LINE_MAX 256

// ----------------------------------------------------------------------------
// Utilitaires de texte
// ----------------------------------------------------------------------------

// Saute les espaces au début d'une chaîne
static const char *skip_spaces(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }

// Compare le mot commençant en `s` (fini par espace ou fin de chaîne) avec `word`
static int word_is(const char *s, const char *word) {
    while (*word) { if (*s != *word) return 0; s++; word++; }
    return *s == 0 || *s == ' ' || *s == '\t';
}

// ----------------------------------------------------------------------------
// VALEURS pour `return`  (nom, aide, fonction qui affiche la valeur)
// ----------------------------------------------------------------------------

// Heure : pas encore de source d'horloge (RTC) dans l'OS
static void val_time(void)   { kputs("indisponible (pas encore d'horloge)\n"); }

// Disposition du clavier : uniquement "fr" ou "us"
static void val_keymap(void) { kprintf("%s\n", kbd_layout_name()); }

// RAM utilisable, en Mo
static void val_ram(void)    { kprintf("%lu Mo\n", mem_usable_bytes() >> 20); }

// Taille de l'écran et de la grille de texte
static void val_screen(void) {
    if (!console_ready()) { kputs("pas d'ecran graphique\n"); return; }
    kprintf("%u colonnes x %u lignes\n", console_cols(), console_rows());
}

typedef struct { const char *name; const char *help; void (*print)(void); } Value;

static const Value values[] = {
    { "time",   "heure courante (plus tard)",        val_time   },
    { "ram",    "RAM utilisable",                    val_ram    },
    { "screen", "taille de la console",              val_screen },
    { "keymap", "disposition clavier (fr ou us)",    val_keymap },
};
#define N_VALUES (sizeof(values) / sizeof(values[0]))

// ----------------------------------------------------------------------------
// COMMANDES  (nom, aide, fonction ; `args` = le reste de la ligne, sans le nom)
// ----------------------------------------------------------------------------

// help : liste les commandes
static void cmd_help(const char *args);
// clear : efface l'écran
static void cmd_clear(const char *args) { (void)args; console_clear(); }

// keymap          -> aide de la commande
// keymap fr | us  -> change la disposition (AZERTY / QWERTY)
// (la disposition actuelle s'obtient avec : return keymap)
static void cmd_keymap(const char *args) {
    args = skip_spaces(args);
    char name[4]; int n = 0;
    while (*args && *args != ' ' && *args != '\t' && n < 3) name[n++] = *args++;
    name[n] = 0;
    if (n && kbd_set_layout(name)) { kprintf("disposition : %s\n", kbd_layout_name()); return; }
    if (n) kputs("disposition inconnue.\n");               // argument donné mais ni fr ni us
    kputs("usage : keymap fr | us\n"
          "  fr  AZERTY (par defaut)\n"
          "  us  QWERTY\n"
          "disposition actuelle : return keymap\n");
}

// return "texte"  -> affiche le texte
// return valeur   -> affiche la valeur (voir tableau `values`)
static void cmd_return(const char *args) {
    args = skip_spaces(args);
    if (*args == 0) {                                     // pas d'argument : mode d'emploi
        kputs("usage : return \"texte\"  ou  return <valeur>\nvaleurs :\n");
        for (unsigned i = 0; i < N_VALUES; i++) kprintf("  %s - %s\n", values[i].name, values[i].help);
        return;
    }
    if (*args == '"') {                                   // texte entre guillemets
        args++;
        while (*args && *args != '"') kputc(*args++);     // s'arrête au guillemet fermant
        kputc('\n');
        return;
    }
    for (unsigned i = 0; i < N_VALUES; i++)               // sinon : nom de valeur
        if (word_is(args, values[i].name)) { values[i].print(); return; }
    kputs("valeur inconnue (pour du texte, utilise des guillemets). Tape : return\n");
}

typedef struct { const char *name; const char *help; void (*run)(const char *args); } Command;

static const Command commands[] = {
    { "help",   "liste les commandes",                       cmd_help   },
    { "clear",  "efface l'ecran",                            cmd_clear  },
    { "keymap", "disposition clavier : keymap fr | us",      cmd_keymap },
    { "fs",     "fichiers Ray64 (topics/themes) : fs help",  cmd_fs     },
    { "cd",     "entre dans un theme/topic (raccourci fs cd)", cmd_cd   },
    { "ls",     "contenu de l'emplacement (raccourci fs ls)", cmd_ls    },
    { "return", "affiche un texte ou une valeur (return)",   cmd_return },
};
#define N_COMMANDS (sizeof(commands) / sizeof(commands[0]))

static void cmd_help(const char *args) {
    (void)args;
    for (unsigned i = 0; i < N_COMMANDS; i++) kprintf("  %-8s %s\n", commands[i].name, commands[i].help);
}

// ----------------------------------------------------------------------------
// Lecture d'une ligne (avec écho et effacement)
// ----------------------------------------------------------------------------
static void read_line(char *buf, int max) {
    int len = 0, esc = 0;
    for (;;) {
        console_cursor_show();                            // curseur visible tant qu'on attend
        int c = input_getc_nb();
        if (c < 0) { __asm__ volatile("yield"); continue; }

        if (esc == 1) { esc = (c == '[') ? 2 : 0; continue; }           // séquence ESC [ x (flèches...) : ignorée
        if (esc == 2) { if (c >= 0x40 && c <= 0x7E) esc = 0; continue; }
        if (c == 0x1B) { esc = 1; continue; }

        if (c == '\r' || c == '\n') { kputc('\n'); buf[len] = 0; return; }   // Entrée

        if (c == 0x7F || c == 0x08) {                     // Retour arrière
            if (len == 0) continue;
            while (len > 0 && ((uint8_t)buf[len - 1] & 0xC0) == 0x80) len--; // saute les octets de suite UTF-8
            len--;                                        // puis l'octet de tête : un caractère entier est effacé
            kputs("\b \b");                               // recule, écrase par un espace, recule
            continue;
        }
        if (c >= 0x20 && len < max - 1) { buf[len++] = (char)c; kputc((char)c); }  // caractère normal
    }
}

// ----------------------------------------------------------------------------
// Exécution d'une ligne : "<commande> <arguments>"
// ----------------------------------------------------------------------------
static void execute(const char *line) {
    line = skip_spaces(line);
    if (*line == 0) return;                               // ligne vide
    for (unsigned i = 0; i < N_COMMANDS; i++) {
        if (word_is(line, commands[i].name)) {
            const char *args = line;
            while (*args && *args != ' ' && *args != '\t') args++;   // saute le nom
            commands[i].run(args);
            return;
        }
    }
    kputs("commande inconnue. Tape : help\n");
}

void shell_run(void) {
    char line[LINE_MAX];
    kputs("\nTerminal minimal. Tape 'help'.\n");
    for (;;) {
        fs_print_prompt();                               // emplacement Ray64 courant puis "> "
        console_set_color(CON_LGRAY, CON_BLACK);
        read_line(line, LINE_MAX);
        execute(line);
    }
}
