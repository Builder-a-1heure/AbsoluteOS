// ============================================================================
// ramdisk.c — Périphérique bloc stocké dans un tableau en RAM
// ============================================================================
#include "ramdisk.h"
#include "kstring.h"

static uint8_t g_ram[RAMDISK_BYTES] __attribute__((aligned(4096)));    // dans .bss : vide au démarrage

static int ram_read(BlockDev *d, uint64_t lba, void *buf) {
    (void)d;
    memcpy(buf, g_ram + lba * BLK_SIZE, BLK_SIZE);
    return 0;
}

static int ram_write(BlockDev *d, uint64_t lba, const void *buf) {
    (void)d;
    memcpy(g_ram + lba * BLK_SIZE, buf, BLK_SIZE);
    return 0;
}

static BlockDev g_dev = { "ramdisk", RAMDISK_BYTES / BLK_SIZE, ram_read, ram_write, 0, 0 };

BlockDev *ramdisk_dev(void) { return &g_dev; }
