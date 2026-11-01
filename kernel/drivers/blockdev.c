// ============================================================================
// blockdev.c — Contrôles communs à tous les périphériques bloc
// ============================================================================
#include "blockdev.h"

int blk_read(BlockDev *d, uint64_t lba, void *buf) {
    if (!d || lba >= d->blocks) return -1;
    return d->read(d, lba, buf);
}

// Écriture normale : la zone système [ro_start, ro_end) est en lecture seule, quoi qu'on demande
int blk_write(BlockDev *d, uint64_t lba, const void *buf) {
    if (!d || lba >= d->blocks) return -1;
    if (lba >= d->ro_start && lba < d->ro_end) return -1;
    return d->write(d, lba, buf);
}

// Écriture qui ignore la protection : uniquement pour installer/mettre à jour les fichiers de l'OS
int blk_write_unprotected(BlockDev *d, uint64_t lba, const void *buf) {
    if (!d || lba >= d->blocks) return -1;
    return d->write(d, lba, buf);
}
