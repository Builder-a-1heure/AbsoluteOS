// ============================================================================
// blockdev.h — Périphérique bloc générique (blocs de 512 octets)
// Un pilote (disque RAM, virtio-blk, NVMe...) remplit cette structure ;
// le système de fichiers ne connaît que cette interface.
// ============================================================================
#pragma once
#include <stdint.h>

#define BLK_SIZE 512

typedef struct BlockDev {
    const char *name;
    uint64_t    blocks;                                                     // nombre de blocs du périphérique
    int (*read)(struct BlockDev *d, uint64_t lba, void *buf);               // lit 1 bloc ; 0 = OK
    int (*write)(struct BlockDev *d, uint64_t lba, const void *buf);        // écrit 1 bloc ; 0 = OK
    uint64_t    ro_start, ro_end;                                           // zone [start,end) protégée en écriture
} BlockDev;

int blk_read(BlockDev *d, uint64_t lba, void *buf);
int blk_write(BlockDev *d, uint64_t lba, const void *buf);                  // REFUSE la zone protégée
int blk_write_unprotected(BlockDev *d, uint64_t lba, const void *buf);      // réservé à l'installateur système
