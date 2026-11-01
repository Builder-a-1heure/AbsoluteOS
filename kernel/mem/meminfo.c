// ============================================================================
// meminfo.c — Parcourt la carte mémoire UEFI et additionne la RAM utilisable
// ============================================================================
#include "meminfo.h"
#include "efi.h"

static uint64_t g_usable, g_top;
static uint32_t g_regions;

void mem_init(const BootInfo *bi) {
    g_usable = g_top = 0; g_regions = 0;
    for (uint64_t off = 0; off < bi->mmap_size; off += bi->mmap_desc_size) {
        // On avance de desc_size (donné par le firmware), PAS de sizeof(descripteur)
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)(bi->mmap + off);
        uint64_t bytes = d->pages * 4096;
        uint32_t t = d->type;
        // Utilisable : RAM libre + restes du loader et des services de boot (UEFI n'en a plus besoin)
        if (t == EFI_MEM_CONVENTIONAL || t == EFI_MEM_BS_CODE || t == EFI_MEM_BS_DATA ||
            t == EFI_MEM_LOADER_CODE || t == EFI_MEM_LOADER_DATA) {
            g_usable += bytes;
            if (d->phys_start + bytes > g_top) g_top = d->phys_start + bytes;
        }
        g_regions++;
    }
}

uint64_t mem_usable_bytes(void) { return g_usable; }
uint64_t mem_top_address(void)  { return g_top; }
uint32_t mem_region_count(void) { return g_regions; }
