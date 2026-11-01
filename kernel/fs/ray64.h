// ============================================================================
// ray64.h — Ray64 by Sora : système de fichiers par TOPICS et THÈMES
//
// Disque découpé en blocs de 512 octets. Les données d'un fichier sont chaînées
// comme en FAT (une table donne, pour chaque bloc, le bloc suivant).
// Les fichiers ne sont pas rangés dans des dossiers mais reliés à des TOPICS :
//   - un THÈME regroupe des topics (app, documents, photos, music...)
//   - un TOPIC appartient à un thème ; "documents/cv" et "photos/cv" sont deux topics
//   - un FICHIER peut être relié à plusieurs topics (donc à plusieurs thèmes)
//   - ouvrir un topic = lister tous les fichiers qui lui sont reliés
//   - un fichier relié à plus aucun topic est supprimé
//
// Zone SYSTÈME : blocs réservés à l'OS, invisibles (aucun topic ne peut les
// atteindre) et protégés en écriture au niveau du périphérique bloc.
// ============================================================================
#pragma once
#include <stdint.h>
#include "blockdev.h"

// ---- Capacités fixes de la version 1 ----
#define R64_MAX_THEMES  32
#define R64_MAX_TOPICS  512
#define R64_MAX_FILES   2048
#define R64_MAX_LINKS   8192
#define R64_MAX_SAVE_TOPICS 8          // topics max par appel de sauvegarde

// ---- Codes de retour ----
enum {
    R64_OK = 0,
    R64_ERR_IO = -1,        // erreur du périphérique
    R64_ERR_NOSPACE = -2,   // plus de place (blocs, fichiers ou liens)
    R64_ERR_EXISTS = -3,    // existe déjà
    R64_ERR_NOTFOUND = -4,  // introuvable
    R64_ERR_DENIED = -5,    // interdit par les règles du thème/topic
    R64_ERR_INVALID = -6,   // nom ou argument invalide
    R64_ERR_FORMAT = -7,    // pas de volume Ray64 valide
};

// ---- Enregistrements (tables en RAM, recopiées sur disque) ----
#define THEME_RESTRICTED 1     // thème où les topics ne se créent pas librement (app)
#define TOPIC_LIB        1     // topic des bibliothèques partagées
#define TOPIC_APP        2     // topic d'une application
#define TOPIC_PROTECTED  4     // ne peut pas être supprimé

typedef struct { char name[24]; uint32_t flags; uint32_t pad; } R64Theme;                       // 32 octets
typedef struct { char name[24]; uint16_t theme; uint16_t flags; uint32_t pad; } R64Topic;       // 32 octets
typedef struct { char name[32]; uint32_t size; uint32_t first; uint32_t flags; uint32_t pad[5]; } R64File;  // 64 octets
typedef struct { uint32_t file; uint16_t topic; uint16_t flags; } R64Link;                      // 8 octets

typedef struct {
    uint64_t total_blocks, free_blocks;
    uint32_t sys_blocks, data_start;
    uint32_t themes, topics, files, links;
} R64Info;

// ---- Volume ----
int  ray64_format(BlockDev *dev);          // crée un volume vide avec les thèmes par défaut
int  ray64_mount(BlockDev *dev);
int  ray64_mounted(void);
void ray64_info(R64Info *out);
const char *ray64_strerror(int err);

// ---- Thèmes et topics ----
int  ray64_theme_find(const char *name);                 // id ou ERR_NOTFOUND
int  ray64_theme_create(const char *name);               // id ou erreur
int  ray64_topic_find(int theme, const char *name);
int  ray64_topic_resolve(const char *path);              // "theme/topic" -> id ou erreur
int  ray64_topic_create(const char *path);               // refuse dans un thème restreint (app)
int  ray64_topic_delete(const char *path);               // seulement si vide et non protégé
int  ray64_app_install(const char *app);                 // crée le topic app/<app> (réservé à l'installateur)
const R64Theme *ray64_theme(int id);                     // NULL si inutilisé
const R64Topic *ray64_topic(int id);
const R64File  *ray64_file(int id);
const R64Link  *ray64_link(int idx);                     // NULL si inutilisé
int  ray64_topic_file_count(int topic);

// ---- Fichiers ----
int  ray64_save(const char *name, const void *data, uint32_t len, const int *topics, int ntopics);
int  ray64_find_in_topic(int topic, const char *name);   // id fichier ou ERR_NOTFOUND
int  ray64_read(int file, uint32_t offset, void *buf, uint32_t n);   // octets lus, ou erreur
int  ray64_link_file(int file, int topic);               // relie un fichier existant à un autre topic
int  ray64_unlink(int file, int topic);                  // 0 = délié ; 1 = délié ET fichier supprimé (plus aucun topic)
