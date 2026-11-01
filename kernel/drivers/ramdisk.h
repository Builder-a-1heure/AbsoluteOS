// ============================================================================
// ramdisk.h — Disque en RAM (contenu perdu à l'extinction)
// Sert de support pour développer Ray64 en attendant un pilote de disque réel.
// ============================================================================
#pragma once
#include "blockdev.h"

#define RAMDISK_BYTES (4u * 1024 * 1024)      // 4 Mo = 8192 blocs

BlockDev *ramdisk_dev(void);
