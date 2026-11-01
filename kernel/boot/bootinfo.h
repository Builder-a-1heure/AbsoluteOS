// ============================================================================
// bootinfo.h — Ce que le bootloader transmet au noyau
// Rempli par efi_main() (boot_efi.c) avant de quitter UEFI, lu par kmain().
// ============================================================================
#pragma once
#include <stdint.h>

typedef struct {
    // ---- Écran (framebuffer linéaire fourni par le GOP) ----
    uint64_t fb_base;        // adresse physique du premier pixel
    uint64_t fb_size;        // taille en octets
    uint32_t fb_width;       // largeur en pixels
    uint32_t fb_height;      // hauteur en pixels
    uint32_t fb_pitch;       // octets par ligne (peut être > width*4)
    uint32_t fb_format;      // 0 = octets R,G,B,x ; 1 = octets B,G,R,x

    // ---- Carte mémoire UEFI (tableau de EFI_MEMORY_DESCRIPTOR) ----
    uint64_t mmap;           // adresse du tableau
    uint64_t mmap_size;      // taille totale en octets
    uint64_t mmap_desc_size; // taille d'UN descripteur (à utiliser comme pas)

    // ---- ACPI : décrit les cœurs, l'interruption controller, l'UART... (étape suivante)
    uint64_t acpi_rsdp;      // adresse physique du RSDP (0 si absent)
} BootInfo;

// Défini dans boot.S : installe pile + vecteurs puis appelle kmain(info)
void kernel_entry(BootInfo *info) __attribute__((noreturn));
