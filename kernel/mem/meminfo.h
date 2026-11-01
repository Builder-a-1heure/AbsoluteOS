// ============================================================================
// meminfo.h — Informations sur la RAM, calculées depuis la carte mémoire UEFI
// ============================================================================
#pragma once
#include <stdint.h>
#include "bootinfo.h"

void     mem_init(const BootInfo *bi);
uint64_t mem_usable_bytes(void);     // RAM libre ou récupérable (hors zones réservées au firmware)
uint64_t mem_top_address(void);      // plus haute adresse physique de RAM utilisable
uint32_t mem_region_count(void);     // nombre de zones dans la carte UEFI
