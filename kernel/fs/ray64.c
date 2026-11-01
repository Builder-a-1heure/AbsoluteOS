// ============================================================================
// ray64.c — Ray64 by Sora : implémentation
//
// DISPOSITION SUR LE DISQUE (en numéros de bloc, LBA) :
//   0                superblock (description du volume)
//   sys_start..      ZONE SYSTÈME : fichiers de l'OS, invisible et protégée en écriture
//   fat_start..      table FAT : une entrée de 4 octets par bloc du disque
//                      0 = libre, 0xFFFFFFFF = fin de chaîne, 0xFFFFFFFE = réservé, sinon bloc suivant
//   themes_start..   table des thèmes   (32 octets chacun)
//   topics_start..   table des topics   (32 octets chacun)
//   files_start..    table des fichiers (64 octets chacun)
//   links_start..    table des liens fichier<->topic (8 octets chacun)
//   data_start..     blocs de données des fichiers
//
// Les 4 tables sont lues en RAM au montage ; chaque modification réécrit
// immédiatement le bloc de 512 octets qui contient l'enregistrement changé.
// ============================================================================
#include <stdint.h>
#include "ray64.h"
#include "ray64_priv.h"
#include "kstring.h"

#define FAT_FREE      0x00000000u
#define FAT_RESERVED  0xFFFFFFFEu
#define FAT_END       0xFFFFFFFFu
#define FAT_PER_BLOCK (BLK_SIZE / 4)

// Nombre de blocs occupés par chaque table
#define THEME_BLOCKS ((R64_MAX_THEMES * sizeof(R64Theme)) / BLK_SIZE)
#define TOPIC_BLOCKS ((R64_MAX_TOPICS * sizeof(R64Topic)) / BLK_SIZE)
#define FILE_BLOCKS  ((R64_MAX_FILES  * sizeof(R64File))  / BLK_SIZE)
#define LINK_BLOCKS  ((R64_MAX_LINKS  * sizeof(R64Link))  / BLK_SIZE)

_Static_assert(sizeof(R64Theme) == 32 && sizeof(R64Topic) == 32, "enregistrement theme/topic = 32 octets");
_Static_assert(sizeof(R64File) == 64 && sizeof(R64Link) == 8, "enregistrement fichier = 64, lien = 8");

// ---- Superblock : un bloc de 512 octets ----
typedef union {
    struct {
        char     magic[16];                // "Ray64 by Sora"
        uint32_t version, block_size;
        uint64_t total_blocks;
        uint32_t sys_start, sys_blocks;
        uint32_t fat_start, fat_blocks;
        uint32_t themes_start, topics_start, files_start, links_start, data_start;
        uint32_t max_themes, max_topics, max_files, max_links;   // capacités du format (vérifiées au montage)
        uint32_t free_blocks, free_hint;
        uint32_t checksum;
    } f;
    uint8_t raw[BLK_SIZE];
} Super;

static const char MAGIC[16] = "Ray64 by Sora";

// ---- État du volume monté ----
static BlockDev *g_dev;
static int       g_mounted;
static Super     g_sb;
static R64Theme  g_themes[R64_MAX_THEMES];
static R64Topic  g_topics[R64_MAX_TOPICS];
static R64File   g_files[R64_MAX_FILES];
static R64Link   g_links[R64_MAX_LINKS];

// Cache d'un bloc de la FAT (écriture immédiate)
static uint32_t g_fatbuf[FAT_PER_BLOCK] __attribute__((aligned(8)));
static uint32_t g_fat_lba = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// Outils
// ---------------------------------------------------------------------------

const char *ray64_strerror(int e) {
    switch (e) {
    case R64_OK:           return "ok";
    case R64_ERR_IO:       return "erreur disque";
    case R64_ERR_NOSPACE:  return "plus de place";
    case R64_ERR_EXISTS:   return "existe deja";
    case R64_ERR_NOTFOUND: return "introuvable";
    case R64_ERR_DENIED:   return "interdit";
    case R64_ERR_INVALID:  return "nom ou argument invalide";
    case R64_ERR_FORMAT:   return "pas de volume Ray64 valide";
    }
    return "erreur inconnue";
}

// Somme de contrôle du superblock (FNV-1a sur les 512 octets, champ checksum à 0)
static uint32_t sb_checksum(Super *s) {
    uint32_t saved = s->f.checksum, h = 2166136261u;
    s->f.checksum = 0;
    for (int i = 0; i < BLK_SIZE; i++) { h ^= s->raw[i]; h *= 16777619u; }
    s->f.checksum = saved;
    return h;
}

static int sb_flush(void) {
    g_sb.f.checksum = sb_checksum(&g_sb);
    return blk_write(g_dev, 0, g_sb.raw) ? R64_ERR_IO : R64_OK;
}

// Réécrit le bloc de 512 octets d'une table en RAM qui contient l'enregistrement idx
static int flush_rec(uint32_t table_start, void *table, uint32_t rec_size, uint32_t idx) {
    uint32_t blk = (idx * rec_size) / BLK_SIZE;
    return blk_write(g_dev, table_start + blk, (uint8_t *)table + blk * BLK_SIZE) ? R64_ERR_IO : R64_OK;
}

// Noms : non vides, sans '/' ni caractère de contrôle, longueur < max
static int valid_name(const char *s, size_t max) {
    size_t n = strlen(s);
    if (n == 0 || n >= max) return 0;
    for (size_t i = 0; i < n; i++) if ((uint8_t)s[i] < 0x20 || s[i] == '/') return 0;
    return 1;
}

static void copy_name(char *dst, size_t size, const char *src) {
    memset(dst, 0, size);
    memcpy(dst, src, strlen(src));
}

// ---------------------------------------------------------------------------
// FAT : chaînes de blocs
// ---------------------------------------------------------------------------
static int fat_load(uint32_t block) {
    uint32_t lba = g_sb.f.fat_start + block / FAT_PER_BLOCK;
    if (g_fat_lba == lba) return 0;
    if (blk_read(g_dev, lba, g_fatbuf)) return -1;
    g_fat_lba = lba;
    return 0;
}

static uint32_t fat_get(uint32_t block) {
    if (fat_load(block)) return FAT_RESERVED;
    return g_fatbuf[block % FAT_PER_BLOCK];
}

static int fat_set(uint32_t block, uint32_t value) {
    if (fat_load(block)) return -1;
    g_fatbuf[block % FAT_PER_BLOCK] = value;
    return blk_write(g_dev, g_fat_lba, g_fatbuf);
}

// Réserve un bloc libre dans la zone de données (recherche circulaire à partir d'un repère)
static int alloc_block(uint32_t *out) {
    uint32_t start = g_sb.f.data_start, span = (uint32_t)g_sb.f.total_blocks - start;
    if (g_sb.f.free_blocks == 0 || span == 0) return R64_ERR_NOSPACE;
    uint32_t hint = g_sb.f.free_hint;
    if (hint < start || hint >= start + span) hint = start;
    for (uint32_t i = 0; i < span; i++) {
        uint32_t b = start + (hint - start + i) % span;
        if (fat_get(b) == FAT_FREE) {
            if (fat_set(b, FAT_END)) return R64_ERR_IO;
            g_sb.f.free_blocks--;
            g_sb.f.free_hint = b + 1;
            *out = b;
            return R64_OK;
        }
    }
    return R64_ERR_NOSPACE;
}

// Libère toute une chaîne de blocs
static void free_chain(uint32_t first) {
    uint32_t b = first, guard = 0;
    while (b != FAT_END && b != FAT_FREE && b != FAT_RESERVED && b < g_sb.f.total_blocks && guard++ < g_sb.f.total_blocks) {
        uint32_t next = fat_get(b);
        fat_set(b, FAT_FREE);
        g_sb.f.free_blocks++;
        b = next;
    }
}

// ---------------------------------------------------------------------------
// Formatage et montage
// ---------------------------------------------------------------------------

// Écrit n blocs de zéros à partir de lba
static int zero_blocks(uint32_t lba, uint32_t n) {
    uint8_t z[BLK_SIZE];
    memset(z, 0, sizeof(z));
    for (uint32_t i = 0; i < n; i++) if (blk_write(g_dev, lba + i, z)) return -1;
    return 0;
}

int ray64_format(BlockDev *dev) {
    g_dev = dev; g_mounted = 0; g_fat_lba = 0xFFFFFFFFu;
    uint64_t total = dev->blocks;
    if (total < 1024) return R64_ERR_NOSPACE;
    dev->ro_start = dev->ro_end = 0;                 // pendant le formatage, rien n'est protégé

    memset(&g_sb, 0, sizeof(g_sb));
    memcpy(g_sb.f.magic, MAGIC, sizeof(MAGIC));
    g_sb.f.version = 1;
    g_sb.f.block_size = BLK_SIZE;
    g_sb.f.total_blocks = total;
    g_sb.f.sys_start = 1;
    g_sb.f.sys_blocks = (uint32_t)(total / 16);      // 1/16e du volume réservé à l'OS
    if (g_sb.f.sys_blocks < 64) g_sb.f.sys_blocks = 64;
    g_sb.f.fat_start = g_sb.f.sys_start + g_sb.f.sys_blocks;
    g_sb.f.fat_blocks = (uint32_t)((total * 4 + BLK_SIZE - 1) / BLK_SIZE);
    g_sb.f.themes_start = g_sb.f.fat_start + g_sb.f.fat_blocks;
    g_sb.f.topics_start = g_sb.f.themes_start + THEME_BLOCKS;
    g_sb.f.files_start  = g_sb.f.topics_start + TOPIC_BLOCKS;
    g_sb.f.links_start  = g_sb.f.files_start + FILE_BLOCKS;
    g_sb.f.data_start   = g_sb.f.links_start + LINK_BLOCKS;
    g_sb.f.max_themes = R64_MAX_THEMES; g_sb.f.max_topics = R64_MAX_TOPICS;
    g_sb.f.max_files = R64_MAX_FILES;   g_sb.f.max_links = R64_MAX_LINKS;
    if (g_sb.f.data_start + 16 >= total) return R64_ERR_NOSPACE;
    g_sb.f.free_blocks = (uint32_t)total - g_sb.f.data_start;
    g_sb.f.free_hint = g_sb.f.data_start;

    // 1. invalide l'ancien volume (un formatage interrompu ne doit pas ressembler à un volume valide)
    if (zero_blocks(0, 1)) return R64_ERR_IO;

    // 2. tables vides
    if (zero_blocks(g_sb.f.themes_start, g_sb.f.data_start - g_sb.f.themes_start)) return R64_ERR_IO;

    // 3. FAT : blocs de métadonnées réservés, données libres
    uint32_t fat[FAT_PER_BLOCK];
    for (uint32_t fb = 0; fb < g_sb.f.fat_blocks; fb++) {
        for (uint32_t i = 0; i < FAT_PER_BLOCK; i++) {
            uint64_t b = (uint64_t)fb * FAT_PER_BLOCK + i;
            fat[i] = (b < g_sb.f.data_start || b >= total) ? FAT_RESERVED : FAT_FREE;
        }
        if (blk_write(g_dev, g_sb.f.fat_start + fb, fat)) return R64_ERR_IO;
    }

    // 4. thèmes et topic par défaut
    memset(g_themes, 0, sizeof(g_themes)); memset(g_topics, 0, sizeof(g_topics));
    memset(g_files, 0, sizeof(g_files));   memset(g_links, 0, sizeof(g_links));
    copy_name(g_themes[0].name, sizeof(g_themes[0].name), "app");       g_themes[0].flags = THEME_RESTRICTED;
    copy_name(g_themes[1].name, sizeof(g_themes[1].name), "documents");
    copy_name(g_themes[2].name, sizeof(g_themes[2].name), "photos");
    copy_name(g_themes[3].name, sizeof(g_themes[3].name), "music");
    copy_name(g_topics[0].name, sizeof(g_topics[0].name), "lib");       // bibliothèques partagées
    g_topics[0].theme = 0;
    g_topics[0].flags = TOPIC_LIB | TOPIC_PROTECTED;
    if (blk_write(g_dev, g_sb.f.themes_start, g_themes)) return R64_ERR_IO;
    if (blk_write(g_dev, g_sb.f.topics_start, g_topics)) return R64_ERR_IO;

    // 5. le superblock en dernier : le volume n'existe qu'une fois tout le reste écrit
    g_mounted = 1;
    g_dev->ro_start = g_sb.f.sys_start;                        // la zone système devient protégée
    g_dev->ro_end   = g_sb.f.sys_start + g_sb.f.sys_blocks;
    return sb_flush();
}

int ray64_mount(BlockDev *dev) {
    g_dev = dev; g_mounted = 0; g_fat_lba = 0xFFFFFFFFu;
    if (blk_read(dev, 0, g_sb.raw)) return R64_ERR_IO;
    if (memcmp(g_sb.f.magic, MAGIC, sizeof(MAGIC)) != 0) return R64_ERR_FORMAT;
    if (g_sb.f.checksum != sb_checksum(&g_sb)) return R64_ERR_FORMAT;
    if (g_sb.f.version != 1 || g_sb.f.block_size != BLK_SIZE || g_sb.f.total_blocks > dev->blocks) return R64_ERR_FORMAT;
    if (g_sb.f.max_themes != R64_MAX_THEMES || g_sb.f.max_topics != R64_MAX_TOPICS ||
        g_sb.f.max_files != R64_MAX_FILES || g_sb.f.max_links != R64_MAX_LINKS) return R64_ERR_FORMAT;

    // Charge les 4 tables en RAM, bloc par bloc
    for (uint32_t i = 0; i < THEME_BLOCKS; i++) if (blk_read(dev, g_sb.f.themes_start + i, (uint8_t *)g_themes + i * BLK_SIZE)) return R64_ERR_IO;
    for (uint32_t i = 0; i < TOPIC_BLOCKS; i++) if (blk_read(dev, g_sb.f.topics_start + i, (uint8_t *)g_topics + i * BLK_SIZE)) return R64_ERR_IO;
    for (uint32_t i = 0; i < FILE_BLOCKS;  i++) if (blk_read(dev, g_sb.f.files_start  + i, (uint8_t *)g_files  + i * BLK_SIZE)) return R64_ERR_IO;
    for (uint32_t i = 0; i < LINK_BLOCKS;  i++) if (blk_read(dev, g_sb.f.links_start  + i, (uint8_t *)g_links  + i * BLK_SIZE)) return R64_ERR_IO;

    dev->ro_start = g_sb.f.sys_start;                          // protège la zone système
    dev->ro_end   = g_sb.f.sys_start + g_sb.f.sys_blocks;
    g_mounted = 1;
    return R64_OK;
}

int ray64_mounted(void) { return g_mounted; }

void ray64_info(R64Info *o) {
    memset(o, 0, sizeof(*o));
    if (!g_mounted) return;
    o->total_blocks = g_sb.f.total_blocks; o->free_blocks = g_sb.f.free_blocks;
    o->sys_blocks = g_sb.f.sys_blocks;     o->data_start = g_sb.f.data_start;
    for (int i = 0; i < R64_MAX_THEMES; i++) if (g_themes[i].name[0]) o->themes++;
    for (int i = 0; i < R64_MAX_TOPICS; i++) if (g_topics[i].name[0]) o->topics++;
    for (int i = 0; i < R64_MAX_FILES;  i++) if (g_files[i].name[0])  o->files++;
    for (int i = 0; i < R64_MAX_LINKS;  i++) if (g_links[i].flags & 1) o->links++;
}

// ---------------------------------------------------------------------------
// Thèmes et topics
// ---------------------------------------------------------------------------
const R64Theme *ray64_theme(int id) { return (g_mounted && id >= 0 && id < R64_MAX_THEMES && g_themes[id].name[0]) ? &g_themes[id] : 0; }
const R64Topic *ray64_topic(int id) { return (g_mounted && id >= 0 && id < R64_MAX_TOPICS && g_topics[id].name[0]) ? &g_topics[id] : 0; }
const R64File  *ray64_file(int id)  { return (g_mounted && id >= 0 && id < R64_MAX_FILES  && g_files[id].name[0])  ? &g_files[id]  : 0; }
const R64Link  *ray64_link(int i)   { return (g_mounted && i >= 0 && i < R64_MAX_LINKS && (g_links[i].flags & 1)) ? &g_links[i] : 0; }

int ray64_theme_find(const char *name) {
    if (!g_mounted) return R64_ERR_FORMAT;
    for (int i = 0; i < R64_MAX_THEMES; i++)
        if (g_themes[i].name[0] && strcmp(g_themes[i].name, name) == 0) return i;
    return R64_ERR_NOTFOUND;
}

int ray64_topic_find(int theme, const char *name) {
    if (!g_mounted) return R64_ERR_FORMAT;
    for (int i = 0; i < R64_MAX_TOPICS; i++)
        if (g_topics[i].name[0] && g_topics[i].theme == theme && strcmp(g_topics[i].name, name) == 0) return i;
    return R64_ERR_NOTFOUND;
}

// "theme/topic" -> id du topic
int ray64_topic_resolve(const char *path) {
    char theme[24];
    const char *slash = path;
    while (*slash && *slash != '/') slash++;
    size_t n = (size_t)(slash - path);
    if (*slash != '/' || n == 0 || n >= sizeof(theme)) return R64_ERR_INVALID;
    memcpy(theme, path, n); theme[n] = 0;
    int t = ray64_theme_find(theme);
    if (t < 0) return t;
    return ray64_topic_find(t, slash + 1);
}

int ray64_theme_create(const char *name) {
    if (!g_mounted) return R64_ERR_FORMAT;
    if (!valid_name(name, sizeof(g_themes[0].name))) return R64_ERR_INVALID;
    if (ray64_theme_find(name) >= 0) return R64_ERR_EXISTS;
    for (int i = 0; i < R64_MAX_THEMES; i++)
        if (!g_themes[i].name[0]) {
            memset(&g_themes[i], 0, sizeof(g_themes[i]));
            copy_name(g_themes[i].name, sizeof(g_themes[i].name), name);
            return flush_rec(g_sb.f.themes_start, g_themes, sizeof(R64Theme), i) ? R64_ERR_IO : i;
        }
    return R64_ERR_NOSPACE;
}

// Création interne d'un topic (sans les règles des thèmes restreints)
static int topic_add(int theme, const char *name, uint16_t flags) {
    if (!valid_name(name, sizeof(g_topics[0].name))) return R64_ERR_INVALID;
    if (ray64_topic_find(theme, name) >= 0) return R64_ERR_EXISTS;
    for (int i = 0; i < R64_MAX_TOPICS; i++)
        if (!g_topics[i].name[0]) {
            memset(&g_topics[i], 0, sizeof(g_topics[i]));
            copy_name(g_topics[i].name, sizeof(g_topics[i].name), name);
            g_topics[i].theme = (uint16_t)theme;
            g_topics[i].flags = flags;
            return flush_rec(g_sb.f.topics_start, g_topics, sizeof(R64Topic), i) ? R64_ERR_IO : i;
        }
    return R64_ERR_NOSPACE;
}

// Sépare "theme/topic" en thème (id) et nom de topic
static int split_path(const char *path, int *theme, const char **topic_name) {
    char tn[24];
    const char *slash = path;
    while (*slash && *slash != '/') slash++;
    size_t n = (size_t)(slash - path);
    if (*slash != '/' || n == 0 || n >= sizeof(tn)) return R64_ERR_INVALID;
    memcpy(tn, path, n); tn[n] = 0;
    *theme = ray64_theme_find(tn);
    *topic_name = slash + 1;
    return *theme < 0 ? *theme : R64_OK;
}

int ray64_topic_create(const char *path) {
    int theme; const char *name;
    int r = split_path(path, &theme, &name);
    if (r) return r;
    if (g_themes[theme].flags & THEME_RESTRICTED) return R64_ERR_DENIED;   // app : un topic par application, via l'installateur
    return topic_add(theme, name, 0);
}

// Installateur d'applications : crée le topic de l'app (le nom "lib" est réservé aux bibliothèques partagées)
int ray64_app_install(const char *app) {
    if (!g_mounted) return R64_ERR_FORMAT;
    int theme = ray64_theme_find("app");
    if (theme < 0) return theme;
    if (strcmp(app, "lib") == 0) return R64_ERR_EXISTS;
    return topic_add(theme, app, TOPIC_APP);
}

int ray64_topic_file_count(int topic) {
    int n = 0;
    for (int i = 0; i < R64_MAX_LINKS; i++) if ((g_links[i].flags & 1) && g_links[i].topic == topic) n++;
    return n;
}

int ray64_topic_delete(const char *path) {
    int t = ray64_topic_resolve(path);
    if (t < 0) return t;
    if (g_topics[t].flags & (TOPIC_PROTECTED | TOPIC_APP)) return R64_ERR_DENIED;
    if (ray64_topic_file_count(t) > 0) return R64_ERR_DENIED;              // il faut d'abord vider le topic
    memset(&g_topics[t], 0, sizeof(g_topics[t]));
    return flush_rec(g_sb.f.topics_start, g_topics, sizeof(R64Topic), t);
}

// ---------------------------------------------------------------------------
// Fichiers
// ---------------------------------------------------------------------------
int ray64_find_in_topic(int topic, const char *name) {
    if (!g_mounted) return R64_ERR_FORMAT;
    for (int i = 0; i < R64_MAX_LINKS; i++)
        if ((g_links[i].flags & 1) && g_links[i].topic == topic && strcmp(g_files[g_links[i].file].name, name) == 0)
            return (int)g_links[i].file;
    return R64_ERR_NOTFOUND;
}

// Écrit un lien (fichier, topic) dans une case libre
static int link_add(int file, int topic) {
    for (int i = 0; i < R64_MAX_LINKS; i++)
        if (!(g_links[i].flags & 1)) {
            g_links[i].file = (uint32_t)file;
            g_links[i].topic = (uint16_t)topic;
            g_links[i].flags = 1;
            return flush_rec(g_sb.f.links_start, g_links, sizeof(R64Link), i) ? R64_ERR_IO : R64_OK;
        }
    return R64_ERR_NOSPACE;
}

static int free_links(void) {
    int n = 0;
    for (int i = 0; i < R64_MAX_LINKS; i++) if (!(g_links[i].flags & 1)) n++;
    return n;
}

int ray64_save(const char *name, const void *data, uint32_t len, const int *topics, int nt) {
    if (!g_mounted) return R64_ERR_FORMAT;
    if (!valid_name(name, sizeof(g_files[0].name)) || nt < 1 || nt > R64_MAX_SAVE_TOPICS) return R64_ERR_INVALID;

    // Vérifie les topics : existent, pas en double, pas déjà de fichier de ce nom dedans
    for (int i = 0; i < nt; i++) {
        if (!ray64_topic(topics[i])) return R64_ERR_NOTFOUND;
        for (int j = 0; j < i; j++) if (topics[j] == topics[i]) return R64_ERR_INVALID;
        if (ray64_find_in_topic(topics[i], name) >= 0) return R64_ERR_EXISTS;
    }
    int fid = -1;
    for (int i = 0; i < R64_MAX_FILES; i++) if (!g_files[i].name[0]) { fid = i; break; }
    if (fid < 0 || free_links() < nt) return R64_ERR_NOSPACE;
    uint32_t nblocks = (len + BLK_SIZE - 1) / BLK_SIZE;
    if (nblocks > g_sb.f.free_blocks) return R64_ERR_NOSPACE;

    // Données : une chaîne de blocs, chacun écrit au fur et à mesure
    uint32_t first = FAT_END, prev = FAT_END;
    const uint8_t *src = data;
    for (uint32_t i = 0; i < nblocks; i++) {
        uint32_t b;
        int r = alloc_block(&b);
        if (r) { if (first != FAT_END) free_chain(first); return r; }
        if (prev == FAT_END) first = b; else fat_set(prev, b);                // relie au bloc précédent
        uint8_t buf[BLK_SIZE];
        uint32_t n = len - i * BLK_SIZE;
        if (n > BLK_SIZE) n = BLK_SIZE;
        memset(buf, 0, BLK_SIZE);
        memcpy(buf, src + (size_t)i * BLK_SIZE, n);
        if (blk_write(g_dev, b, buf)) { free_chain(first); return R64_ERR_IO; }
        prev = b;
    }

    // Enregistrement du fichier, puis ses liens
    memset(&g_files[fid], 0, sizeof(g_files[fid]));
    copy_name(g_files[fid].name, sizeof(g_files[fid].name), name);
    g_files[fid].size = len;
    g_files[fid].first = first;
    if (flush_rec(g_sb.f.files_start, g_files, sizeof(R64File), fid)) return R64_ERR_IO;
    for (int i = 0; i < nt; i++) { int r = link_add(fid, topics[i]); if (r) return r; }
    int r = sb_flush();
    return r ? r : fid;
}

int ray64_read(int file, uint32_t offset, void *buf, uint32_t n) {
    const R64File *f = ray64_file(file);
    if (!f) return R64_ERR_NOTFOUND;
    if (offset >= f->size) return 0;
    if (n > f->size - offset) n = f->size - offset;
    uint32_t b = f->first, skip = offset / BLK_SIZE, done = 0;
    while (skip-- && b != FAT_END) b = fat_get(b);                            // saute jusqu'au bloc de départ
    uint32_t in = offset % BLK_SIZE;
    while (done < n && b != FAT_END && b < g_sb.f.total_blocks) {
        uint8_t blk[BLK_SIZE];
        if (blk_read(g_dev, b, blk)) return R64_ERR_IO;
        uint32_t chunk = BLK_SIZE - in;
        if (chunk > n - done) chunk = n - done;
        memcpy((uint8_t *)buf + done, blk + in, chunk);
        done += chunk; in = 0;
        b = fat_get(b);
    }
    return (int)done;
}

int ray64_link_file(int file, int topic) {
    if (!ray64_file(file) || !ray64_topic(topic)) return R64_ERR_NOTFOUND;
    for (int i = 0; i < R64_MAX_LINKS; i++)
        if ((g_links[i].flags & 1) && g_links[i].file == (uint32_t)file && g_links[i].topic == topic) return R64_ERR_EXISTS;
    if (ray64_find_in_topic(topic, g_files[file].name) >= 0) return R64_ERR_EXISTS;   // un autre fichier porte ce nom dans ce topic
    return link_add(file, topic);
}

int ray64_unlink(int file, int topic) {
    if (!ray64_file(file) || !ray64_topic(topic)) return R64_ERR_NOTFOUND;
    int found = -1, remaining = 0;
    for (int i = 0; i < R64_MAX_LINKS; i++) {
        if (!(g_links[i].flags & 1) || g_links[i].file != (uint32_t)file) continue;
        if (g_links[i].topic == topic) found = i; else remaining++;
    }
    if (found < 0) return R64_ERR_NOTFOUND;
    g_links[found].flags = 0;
    if (flush_rec(g_sb.f.links_start, g_links, sizeof(R64Link), found)) return R64_ERR_IO;
    if (remaining > 0) return 0;

    // Plus aucun topic : le fichier disparaît et ses blocs sont rendus
    free_chain(g_files[file].first);
    memset(&g_files[file], 0, sizeof(g_files[file]));
    if (flush_rec(g_sb.f.files_start, g_files, sizeof(R64File), file)) return R64_ERR_IO;
    int r = sb_flush();
    return r ? r : 1;
}

// ---------------------------------------------------------------------------
// Zone système (réservée au noyau, voir ray64_priv.h)
// ---------------------------------------------------------------------------
uint32_t ray64_sys_blocks(void) { return g_mounted ? g_sb.f.sys_blocks : 0; }

int ray64_sys_read(uint32_t index, void *buf) {
    if (!g_mounted || index >= g_sb.f.sys_blocks) return R64_ERR_INVALID;
    return blk_read(g_dev, g_sb.f.sys_start + index, buf) ? R64_ERR_IO : R64_OK;
}

int ray64_sys_write(uint32_t index, const void *buf) {
    if (!g_mounted || index >= g_sb.f.sys_blocks) return R64_ERR_INVALID;
    return blk_write_unprotected(g_dev, g_sb.f.sys_start + index, buf) ? R64_ERR_IO : R64_OK;
}
