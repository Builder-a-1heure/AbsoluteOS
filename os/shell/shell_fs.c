// ============================================================================
// shell_fs.c — Commande `fs` : utilise l'API Ray64 (jamais la zone système)
//
// POUR AJOUTER UNE SOUS-COMMANDE : écris  static void fs_xxx(int argc, char **argv)
// puis ajoute UNE ligne dans le tableau `subcommands`.
// argv[0] est le nom de la sous-commande ; les guillemets groupent un argument.
//
// EMPLACEMENT COURANT (comme un "dossier courant") :
//   /                  racine : on y voit les thèmes
//   /photos            un thème : on y voit ses topics
//   /photos/vacances   un topic : on y voit ses fichiers
// Dans un thème, un topic peut être désigné par son nom seul ("vacances") ;
// dans un topic, les commandes sur un fichier peuvent omettre le topic.
// ============================================================================
#include <stdint.h>
#include "shell_fs.h"
#include "ray64.h"
#include "ramdisk.h"
#include "console.h"
#include "kprintf.h"
#include "kstring.h"

#define MAX_ARGS 12

// Couleurs ANSI (interprétées par la console et par un terminal série)
#define C_RST    "\x1b[0m"
#define C_THEME  "\x1b[96m"      // cyan vif
#define C_TOPIC  "\x1b[93m"      // jaune vif
#define C_FILE   "\x1b[97m"      // blanc vif
#define C_SHARED "\x1b[95m"      // magenta vif
#define C_HERE   "\x1b[92m"      // vert vif
#define C_DIM    "\x1b[90m"      // gris

// ---- Emplacement courant : -1 = non défini ----
static int g_theme = -1;         // thème courant (-1 = racine)
static int g_topic = -1;         // topic courant (-1 = pas dans un topic)

// ---------------------------------------------------------------------------
// Utilitaires
// ---------------------------------------------------------------------------

// Découpe `s` en arguments séparés par des espaces ; "entre guillemets" = un seul argument
static int tokenize(const char *s, char *buf, size_t bufsz, char **argv, int max) {
    size_t n = strlen(s);
    if (n >= bufsz) n = bufsz - 1;
    memcpy(buf, s, n); buf[n] = 0;
    int argc = 0;
    char *p = buf;
    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {                                   // argument entre guillemets
            argv[argc++] = ++p;
            while (*p && *p != '"') p++;
        } else {                                           // argument simple
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p) *p++ = 0;                                  // termine l'argument
    }
    return argc;
}

static void print_topic(int topic) {
    const R64Topic *t = ray64_topic(topic);
    const R64Theme *th = t ? ray64_theme(t->theme) : 0;
    kprintf("%s/%s", th ? th->name : "?", t ? t->name : "?");
}

static void print_err(int e) { kprintf("erreur : %s\n", ray64_strerror(e)); }

static int need_volume(void) {
    if (ray64_mounted()) return 1;
    kputs("aucun volume Ray64. Tape : fs format\n");
    return 0;
}

// Si le thème ou le topic courant n'existe plus (supprimé, volume reformaté), on remonte
static void fix_location(void) {
    if (!ray64_mounted()) { g_theme = g_topic = -1; return; }
    if (g_topic >= 0 && !ray64_topic(g_topic)) g_topic = -1;
    if (g_theme >= 0 && !ray64_theme(g_theme)) { g_theme = -1; g_topic = -1; }
}

static int has_slash(const char *s) { for (; *s; s++) if (*s == '/') return 1; return 0; }

// Désigne un topic : "theme/topic" (absolu) ou "topic" (dans le thème courant)
static int resolve_topic(const char *arg) {
    if (*arg == '/') arg++;                                // "/photos/vacances" accepté
    if (has_slash(arg)) return ray64_topic_resolve(arg);
    if (g_theme >= 0) return ray64_topic_find(g_theme, arg);
    return R64_ERR_INVALID;                                // à la racine, il faut theme/topic
}

// Comme resolve_topic mais affiche l'erreur
static int topic_arg(const char *arg) {
    int t = resolve_topic(arg);
    if (t < 0) kprintf("topic introuvable : %s (utilise theme/topic, ou entre d'abord dans un theme)\n", arg);
    return t;
}

// Chemin "theme/topic" pour créer/supprimer un topic (relatif au thème courant si pas de '/')
static const char *topic_path(const char *arg, char *buf, size_t size) {
    if (*arg == '/') arg++;
    if (has_slash(arg) || g_theme < 0) return arg;
    const R64Theme *th = ray64_theme(g_theme);
    if (!th) return arg;
    size_t a = strlen(th->name), b = strlen(arg);
    if (a + 1 + b + 1 > size) return arg;
    memcpy(buf, th->name, a); buf[a] = '/'; memcpy(buf + a + 1, arg, b + 1);
    return buf;
}

// Les autres topics d'un fichier : écrit "a/b, c/d" ; renvoie leur nombre
static int print_other_topics(int file, int except_topic) {
    int n = 0;
    for (int i = 0; i < R64_MAX_LINKS; i++) {
        const R64Link *l = ray64_link(i);
        if (!l || l->file != (uint32_t)file || l->topic == except_topic) continue;
        if (n++) kputs(", ");
        print_topic(l->topic);
    }
    return n;
}

// Combien de topics contient un fichier
static int file_topic_count(int file) {
    int n = 0;
    for (int i = 0; i < R64_MAX_LINKS; i++) { const R64Link *l = ray64_link(i); if (l && l->file == (uint32_t)file) n++; }
    return n;
}

// ---------------------------------------------------------------------------
// Navigation : cd / ls
// ---------------------------------------------------------------------------

// Applique UN segment de chemin à l'emplacement (theme, topic) ; 0 si OK, <0 sinon
static int step(int *theme, int *topic, const char *seg) {
    if (strcmp(seg, ".") == 0 || *seg == 0) return 0;
    if (strcmp(seg, "..") == 0) {
        if (*topic >= 0) *topic = -1; else *theme = -1;    // topic -> thème -> racine
        return 0;
    }
    if (*topic >= 0) return R64_ERR_DENIED;                // un topic ne contient que des fichiers
    if (*theme < 0) { int t = ray64_theme_find(seg); if (t < 0) return t; *theme = t; return 0; }
    int k = ray64_topic_find(*theme, seg);
    if (k < 0) return k;
    *topic = k;
    return 0;
}

// Parcourt un chemin ("photos/vacances", "..", "/documents", "../music"...) depuis (theme, topic)
static int walk(int *theme, int *topic, const char *path) {
    if (*path == '/') { *theme = -1; *topic = -1; path++; }          // chemin absolu : on part de la racine
    char seg[32];
    while (*path) {
        size_t n = 0;
        while (path[n] && path[n] != '/') n++;
        if (n >= sizeof(seg)) return R64_ERR_INVALID;
        memcpy(seg, path, n); seg[n] = 0;
        int r = step(theme, topic, seg);
        if (r) return r;
        path += n;
        if (*path == '/') path++;
    }
    return 0;
}

// Cherche la destination : d'abord relativement à l'emplacement courant, sinon depuis la racine
static int locate(const char *path, int *theme, int *topic) {
    *theme = g_theme; *topic = g_topic;
    int r = walk(theme, topic, path);
    if (r == 0) return 0;
    *theme = -1; *topic = -1;
    return walk(theme, topic, path) == 0 ? 0 : r;
}

static void fs_cd(int argc, char **argv) {
    if (!need_volume()) return;
    fix_location();
    if (argc < 2) { g_theme = g_topic = -1; return; }      // cd seul : retour à la racine
    int th, tp;
    int r = locate(argv[1], &th, &tp);
    if (r == R64_ERR_DENIED) { kputs("un topic ne contient que des fichiers : rien a ouvrir dedans\n"); return; }
    if (r) { kprintf("introuvable : %s\n", argv[1]); return; }
    g_theme = th; g_topic = tp;
}

// Affiche le contenu d'un emplacement
static void list_location(int theme, int topic) {
    if (topic >= 0) {                                      // un topic : ses fichiers
        int n = 0;
        for (int i = 0; i < R64_MAX_LINKS; i++) {
            const R64Link *l = ray64_link(i);
            if (!l || l->topic != topic) continue;
            const R64File *f = ray64_file((int)l->file);
            if (!f) continue;
            n++;
            kprintf("  " C_FILE "%-24s" C_RST " %6u o", f->name, f->size);
            if (file_topic_count((int)l->file) > 1) { kputs("  " C_SHARED "\xe2\x86\x94 "); print_other_topics((int)l->file, topic); kputs(C_RST); }
            kputc('\n');
        }
        if (!n) kputs(C_DIM "  (topic vide)\n" C_RST);
    } else if (theme >= 0) {                               // un thème : ses topics
        int n = 0;
        for (int k = 0; k < R64_MAX_TOPICS; k++) {
            const R64Topic *tp = ray64_topic(k);
            if (!tp || tp->theme != theme) continue;
            n++;
            kprintf("  " C_TOPIC "%-24s" C_RST " %d fichier(s)\n", tp->name, ray64_topic_file_count(k));
        }
        if (!n) kputs(C_DIM "  (theme vide : fs new <topic>)\n" C_RST);
    } else {                                               // la racine : les thèmes
        for (int t = 0; t < R64_MAX_THEMES; t++) {
            const R64Theme *th = ray64_theme(t);
            if (!th) continue;
            int n = 0;
            for (int k = 0; k < R64_MAX_TOPICS; k++) { const R64Topic *tp = ray64_topic(k); if (tp && tp->theme == t) n++; }
            kprintf("  " C_THEME "%-24s" C_RST " %d topic(s)%s\n", th->name, n,
                    (th->flags & THEME_RESTRICTED) ? C_DIM "  [un topic par application]" C_RST : "");
        }
    }
}

static void fs_ls(int argc, char **argv) {
    if (!need_volume()) return;
    fix_location();
    int th = g_theme, tp = g_topic;
    if (argc > 1 && locate(argv[1], &th, &tp) != 0) { kprintf("introuvable : %s\n", argv[1]); return; }
    list_location(th, tp);
}

// Invite du terminal : emplacement courant puis "> "
void fs_print_prompt(void) {
    fix_location();
    if (!ray64_mounted()) { kputs("> "); return; }
    console_set_color(CON_LCYAN, CON_BLACK);
    if (g_theme < 0) kputc('/');
    else {
        kputc('/'); kputs(ray64_theme(g_theme)->name);
        if (g_topic >= 0) { kputc('/'); kputs(ray64_topic(g_topic)->name); }
    }
    console_set_color(CON_LGREEN, CON_BLACK);
    kputs("> ");
}

// ---------------------------------------------------------------------------
// Carte du système de fichiers
// ---------------------------------------------------------------------------

// Parcours des tables : renvoient l'élément utilisé suivant, ou -1
static int next_theme(int from) {
    for (int t = from + 1; t < R64_MAX_THEMES; t++) if (ray64_theme(t)) return t;
    return -1;
}
static int next_topic(int theme, int from) {
    for (int k = from + 1; k < R64_MAX_TOPICS; k++) { const R64Topic *tp = ray64_topic(k); if (tp && tp->theme == theme) return k; }
    return -1;
}
static int next_link(int topic, int from) {
    for (int i = from + 1; i < R64_MAX_LINKS; i++) { const R64Link *l = ray64_link(i); if (l && l->topic == topic) return i; }
    return -1;
}

// fs map [theme] : arbre thème -> topic -> fichiers, avec les liens entre topics
static void fs_map(int argc, char **argv) {
    if (!need_volume()) return;
    fix_location();
    int only = -1;
    if (argc > 1) {
        only = ray64_theme_find(argv[1]);
        if (only < 0) { kprintf("theme introuvable : %s\n", argv[1]); return; }
    }
    R64Info in; ray64_info(&in);
    int shared = 0;
    for (int f = 0; f < R64_MAX_FILES; f++) if (ray64_file(f) && file_topic_count(f) > 1) shared++;

    kprintf(C_HERE "/" C_RST "  Ray64 by Sora : %u themes, %u topics, %u fichiers (%d partage(s)), %u liens%s\n",
            in.themes, in.topics, in.files, shared, in.links, (g_theme < 0 && g_topic < 0) ? C_HERE "  <- ici" C_RST : "");

    for (int t = only >= 0 ? only : next_theme(-1); t >= 0; t = only >= 0 ? -1 : next_theme(t)) {
        int last_theme = (only >= 0) || next_theme(t) < 0;
        kputs(last_theme ? "\xe2\x94\x94\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80 ");     // └─ ou ├─
        kprintf(C_THEME "%s" C_RST "%s\n", ray64_theme(t)->name, (t == g_theme && g_topic < 0) ? C_HERE "  <- ici" C_RST : "");

        for (int k = next_topic(t, -1); k >= 0; k = next_topic(t, k)) {
            int last_topic = next_topic(t, k) < 0;
            kputs(last_theme ? "   " : "\xe2\x94\x82  ");                                 // │ ou vide
            kputs(last_topic ? "\xe2\x94\x94\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80 ");
            kprintf(C_TOPIC "%s" C_RST "%s\n", ray64_topic(k)->name, (k == g_topic) ? C_HERE "  <- ici" C_RST : "");

            for (int i = next_link(k, -1); i >= 0; i = next_link(k, i)) {
                int last_file = next_link(k, i) < 0;
                const R64File *f = ray64_file((int)ray64_link(i)->file);
                if (!f) continue;
                kputs(last_theme ? "   " : "\xe2\x94\x82  ");
                kputs(last_topic ? "   " : "\xe2\x94\x82  ");
                kputs(last_file ? "\xe2\x94\x94\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80 ");
                kprintf(C_FILE "%s" C_RST C_DIM " (%u o)" C_RST, f->name, f->size);
                if (file_topic_count((int)ray64_link(i)->file) > 1) {                     // fichier partagé : on montre ses autres topics
                    kputs("  " C_SHARED "\xe2\x86\x94 ");
                    print_other_topics((int)ray64_link(i)->file, k);
                    kputs(C_RST);
                }
                kputc('\n');
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Volume, thèmes et topics
// ---------------------------------------------------------------------------

static void fs_format(int argc, char **argv) {
    if (ray64_mounted() && !(argc > 1 && strcmp(argv[1], "oui") == 0)) {
        kputs("un volume existe deja : tout serait efface. Confirme avec : fs format oui\n");
        return;
    }
    int r = ray64_format(ramdisk_dev());
    if (r) { print_err(r); return; }
    g_theme = g_topic = -1;                                // retour à la racine
    kputs("volume Ray64 cree (themes : app documents photos music ; topic app/lib)\n");
}

static void fs_info(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!need_volume()) return;
    R64Info i; ray64_info(&i);
    kprintf("Ray64 by Sora - blocs de %d octets\n", BLK_SIZE);
    kprintf("  blocs : %lu au total, %lu libres (%lu Ko libres)\n", i.total_blocks, i.free_blocks, i.free_blocks / 2);
    kprintf("  zone systeme : %u blocs, protegee et invisible\n", i.sys_blocks);
    kprintf("  %u themes, %u topics, %u fichiers, %u liens\n", i.themes, i.topics, i.files, i.links);
}

static void fs_themes(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!need_volume()) return;
    list_location(-1, -1);
}

static void fs_topics(int argc, char **argv) {
    if (!need_volume()) return;
    int only = -1;
    if (argc > 1) {
        only = ray64_theme_find(argv[1]);
        if (only < 0) { kprintf("theme introuvable : %s\n", argv[1]); return; }
    }
    for (int k = 0; k < R64_MAX_TOPICS; k++) {
        const R64Topic *tp = ray64_topic(k);
        if (!tp || (only >= 0 && tp->theme != only)) continue;
        kputs("  "); print_topic(k);
        kprintf("  (%d fichier(s))\n", ray64_topic_file_count(k));
    }
}

// Ouvre un topic : liste tous les fichiers qui lui sont reliés
static void fs_open(int argc, char **argv) {
    if (!need_volume()) return;
    fix_location();
    int t = argc > 1 ? topic_arg(argv[1]) : g_topic;
    if (t < 0) { if (argc < 2) kputs("usage : fs open <theme/topic>\n"); return; }
    print_topic(t); kprintf(" : %d fichier(s)\n", ray64_topic_file_count(t));
    list_location(-1, t);
}

static void fs_new(int argc, char **argv) {
    if (!need_volume()) return;
    fix_location();
    if (argc < 2) { kputs("usage : fs new <theme/topic>   (ou fs new <topic> dans un theme)\n"); return; }
    char path[64];
    int r = ray64_topic_create(topic_path(argv[1], path, sizeof(path)));
    if (r == R64_ERR_DENIED) kputs("theme app : un topic par application (utilise : fs install <app>)\n");
    else if (r < 0) print_err(r);
    else { kputs("topic cree : "); print_topic(r); kputc('\n'); }
}

static void fs_newtheme(int argc, char **argv) {
    if (!need_volume()) return;
    if (argc < 2) { kputs("usage : fs newtheme <nom>\n"); return; }
    int r = ray64_theme_create(argv[1]);
    if (r < 0) print_err(r); else kprintf("theme cree : %s\n", argv[1]);
}

static void fs_rmtopic(int argc, char **argv) {
    if (!need_volume()) return;
    fix_location();
    if (argc < 2) { kputs("usage : fs rmtopic <theme/topic>\n"); return; }
    char path[64];
    int r = ray64_topic_delete(topic_path(argv[1], path, sizeof(path)));
    if (r == R64_ERR_DENIED) kputs("impossible : topic protege, d'application, ou pas vide\n");
    else if (r < 0) print_err(r);
    else kputs("topic supprime\n");
}

static void fs_install(int argc, char **argv) {
    if (!need_volume()) return;
    if (argc < 2) { kputs("usage : fs install <app>\n"); return; }
    int r = ray64_app_install(argv[1]);
    if (r == R64_ERR_EXISTS) kputs("existe deja (le nom 'lib' est reserve aux bibliotheques partagees)\n");
    else if (r < 0) print_err(r);
    else { kputs("topic d'application cree : "); print_topic(r); kputc('\n'); }
}

// ---------------------------------------------------------------------------
// Fichiers
// ---------------------------------------------------------------------------

// Désigne un fichier : "<topic> <nom>" ou, dans un topic, juste "<nom>". argv[first] = 1er argument.
// Renvoie l'id du fichier (et le topic dans *topic) ou <0 (message déjà affiché).
static int file_arg(int argc, char **argv, int first, int *topic) {
    int t; const char *name;
    if (argc - first >= 2)            { t = topic_arg(argv[first]); name = argv[first + 1]; if (t < 0) return t; }
    else if (argc - first == 1 && g_topic >= 0) { t = g_topic; name = argv[first]; }
    else { kputs("il manque le topic (ou entre d'abord dans un topic)\n"); return R64_ERR_INVALID; }
    int f = ray64_find_in_topic(t, name);
    if (f < 0) { kprintf("fichier introuvable dans ce topic : %s\n", name); return f; }
    *topic = t;
    return f;
}

// fs save <nom> "<texte>" [topic ...]   (sans topic : le topic courant)
static void fs_save(int argc, char **argv) {
    if (!need_volume()) return;
    fix_location();
    if (argc < 3 || (argc == 3 && g_topic < 0)) {
        kputs("usage : fs save <nom> \"<texte>\" <theme/topic> [theme/topic ...]   (topic facultatif dans un topic)\n");
        return;
    }
    int topics[R64_MAX_SAVE_TOPICS], nt = 0;
    if (argc == 3) topics[nt++] = g_topic;
    for (int i = 3; i < argc && nt < R64_MAX_SAVE_TOPICS; i++) {
        int t = topic_arg(argv[i]);
        if (t < 0) return;
        topics[nt++] = t;
    }
    int r = ray64_save(argv[1], argv[2], (uint32_t)strlen(argv[2]), topics, nt);
    if (r < 0) print_err(r);
    else kprintf("enregistre : %s (%u octets) dans %d topic(s)\n", argv[1], (unsigned)strlen(argv[2]), nt);
}

// fs cat [topic] <nom>
static void fs_cat(int argc, char **argv) {
    if (!need_volume()) return;
    fix_location();
    int t, f = file_arg(argc, argv, 1, &t);
    if (f < 0) return;
    char buf[128];
    uint32_t off = 0; int n;
    while ((n = ray64_read(f, off, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) kputc(buf[i]);
        off += (uint32_t)n;
    }
    kputc('\n');
}

// fs where [topic] <nom> : tous les topics qui contiennent ce fichier
static void fs_where(int argc, char **argv) {
    if (!need_volume()) return;
    fix_location();
    int t, f = file_arg(argc, argv, 1, &t);
    if (f < 0) return;
    for (int i = 0; i < R64_MAX_LINKS; i++) {
        const R64Link *l = ray64_link(i);
        if (l && l->file == (uint32_t)f) { kputs("  "); print_topic(l->topic); kputc('\n'); }
    }
}

// fs link [topic] <nom> <autre-topic> : relie le fichier à un second topic
static void fs_link(int argc, char **argv) {
    if (!need_volume()) return;
    fix_location();
    if (argc < 3) { kputs("usage : fs link <theme/topic> <nom> <autre theme/topic>   (topic source facultatif dans un topic)\n"); return; }
    int from, f = file_arg(argc - 1, argv, 1, &from);      // le dernier argument est la destination
    if (f < 0) return;
    int to = topic_arg(argv[argc - 1]);
    if (to < 0) return;
    int r = ray64_link_file(f, to);
    if (r < 0) print_err(r); else kputs("relie\n");
}

// fs rm [topic] <nom> : retire le fichier de ce topic (supprimé s'il n'en a plus aucun)
static void fs_rm(int argc, char **argv) {
    if (!need_volume()) return;
    fix_location();
    int t, f = file_arg(argc, argv, 1, &t);
    if (f < 0) return;
    int r = ray64_unlink(f, t);
    if (r < 0) print_err(r);
    else if (r == 1) kputs("retire ; plus aucun topic : fichier supprime\n");
    else kputs("retire de ce topic (le fichier existe encore dans d'autres)\n");
}

// ---------------------------------------------------------------------------
// Table des sous-commandes
// ---------------------------------------------------------------------------
static void fs_help(int argc, char **argv);

typedef struct { const char *name; const char *help; void (*run)(int argc, char **argv); } SubCommand;

static const SubCommand subcommands[] = {
    { "cd",       "cd [chemin]                    entre dans un theme/topic (.. = remonter, / = racine)", fs_cd },
    { "ls",       "ls [chemin]                    contenu de l'emplacement courant",      fs_ls       },
    { "map",      "map [theme]                    carte de tout le volume, liens compris", fs_map     },
    { "format",   "format [oui]                   cree le volume (efface tout)",          fs_format   },
    { "info",     "info                           etat du volume",                        fs_info     },
    { "themes",   "themes                         liste les themes",                      fs_themes   },
    { "topics",   "topics [theme]                 liste les topics",                      fs_topics   },
    { "open",     "open [theme/topic]             fichiers relies a ce topic",            fs_open     },
    { "new",      "new <theme/topic>              cree un topic (nom seul dans un theme)", fs_new     },
    { "newtheme", "newtheme <nom>                 cree un theme",                         fs_newtheme },
    { "rmtopic",  "rmtopic <theme/topic>          supprime un topic vide",                fs_rmtopic  },
    { "install",  "install <app>                  cree le topic d'une application",       fs_install  },
    { "save",     "save <nom> \"texte\" [topic...]  enregistre (topic courant si omis)",   fs_save     },
    { "cat",      "cat [topic] <nom>              affiche un fichier",                    fs_cat      },
    { "where",    "where [topic] <nom>            topics qui contiennent le fichier",     fs_where    },
    { "link",     "link [topic] <nom> <autre>     relie a un autre topic",                fs_link     },
    { "rm",       "rm [topic] <nom>               retire du topic (supprime si dernier)", fs_rm       },
    { "help",     "help                           cette aide",                            fs_help     },
};
#define N_SUB (sizeof(subcommands) / sizeof(subcommands[0]))

static void fs_help(int argc, char **argv) {
    (void)argc; (void)argv;
    kputs("fs - Ray64 by Sora (fichiers relies a des topics, ranges par themes). Raccourcis : cd, ls\n");
    for (unsigned i = 0; i < N_SUB; i++) kprintf("  fs %s\n", subcommands[i].help);
}

// Lance la sous-commande `name` avec les arguments `args` (déjà sans le nom)
static void run_sub(const char *name, int argc, char **argv) {
    for (unsigned i = 0; i < N_SUB; i++)
        if (strcmp(name, subcommands[i].name) == 0) { subcommands[i].run(argc, argv); return; }
    kprintf("sous-commande inconnue : %s (tape : fs help)\n", name);
}

// Point d'entrée : "fs <sous-commande> ..."
void cmd_fs(const char *args) {
    char buf[256];
    char *argv[MAX_ARGS];
    int argc = tokenize(args, buf, sizeof(buf), argv, MAX_ARGS);
    if (argc == 0) { fs_help(0, 0); return; }
    run_sub(argv[0], argc, argv);
}

// Raccourcis : "cd x" = "fs cd x", "ls x" = "fs ls x"
void cmd_cd(const char *args) {
    char buf[256]; char *argv[MAX_ARGS];
    int argc = tokenize(args, buf, sizeof(buf), argv + 1, MAX_ARGS - 1);
    argv[0] = (char *)"cd";
    fs_cd(argc + 1, argv);
}
void cmd_ls(const char *args) {
    char buf[256]; char *argv[MAX_ARGS];
    int argc = tokenize(args, buf, sizeof(buf), argv + 1, MAX_ARGS - 1);
    argv[0] = (char *)"ls";
    fs_ls(argc + 1, argv);
}
