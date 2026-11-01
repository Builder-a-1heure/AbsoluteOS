// ============================================================================
// boot_efi.c — Dernière étape "UEFI" avant le noyau
//
// Appelé par _start (boot.S) tant que le firmware est encore actif :
//   1. récupère le framebuffer (GOP)        -> remplace VESA/VBE de l'x86
//   2. récupère l'adresse ACPI (RSDP)       -> servira pour les cœurs/UART/GIC
//   3. récupère la carte mémoire            -> toute la RAM de la machine
//   4. quitte UEFI (ExitBootServices) et saute dans le noyau
// ============================================================================
#include "efi.h"
#include "bootinfo.h"

static BootInfo g_info;                                    // transmis au noyau (vit dans .bss)
static uint8_t  g_mmap[256 * 1024] __attribute__((aligned(16))); // tampon carte mémoire (statique : pas d'allocation UEFI,
                                                           // car toute allocation invaliderait la clé de la carte)

// Compare deux GUID octet par octet ; renvoie 1 s'ils sont égaux
static int guid_eq(const EFI_GUID *x, const EFI_GUID *y) {
    const uint8_t *a = (const uint8_t *)x, *b = (const uint8_t *)y;
    for (int i = 0; i < (int)sizeof(EFI_GUID); i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

// Point d'entrée C. Ne revient jamais.
EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_BOOT_SERVICES *bs = st->BootServices;

    // ---- 1. Framebuffer : on garde le mode graphique déjà choisi par le firmware
    static const EFI_GUID gop_guid = EFI_GOP_GUID;
    EFI_GOP *gop = 0;
    if (bs->LocateProtocol(&gop_guid, 0, (void **)&gop) == EFI_SUCCESS && gop) {
        g_info.fb_base   = gop->mode->fb_base;
        g_info.fb_size   = gop->mode->fb_size;
        g_info.fb_width  = gop->mode->info->width;
        g_info.fb_height = gop->mode->info->height;
        g_info.fb_pitch  = gop->mode->info->pixels_per_scanline * 4; // 4 octets par pixel
        g_info.fb_format = gop->mode->info->pixel_format;
    }

    // ---- 2. ACPI : cherche le RSDP dans la liste des tables de configuration
    static const EFI_GUID acpi_guid = EFI_ACPI20_GUID;
    for (uint64_t i = 0; i < st->NumberOfTableEntries; i++)
        if (guid_eq(&st->ConfigurationTable[i].guid, &acpi_guid))
            g_info.acpi_rsdp = (uint64_t)st->ConfigurationTable[i].table;

    // ---- 3+4. Carte mémoire puis ExitBootServices.
    // ExitBootServices exige la clé de la carte la PLUS RÉCENTE ; si elle a changé
    // entre-temps, l'appel échoue et on recommence (boucle recommandée par la spec).
    for (int tries = 0; tries < 8; tries++) {
        uint64_t size = sizeof(g_mmap), key, desc_size;
        uint32_t desc_ver;
        if (bs->GetMemoryMap(&size, (EFI_MEMORY_DESCRIPTOR *)g_mmap, &key, &desc_size, &desc_ver) != EFI_SUCCESS)
            break;                                          // tampon trop petit ou autre erreur : on abandonne
        if (bs->ExitBootServices(image, key) == EFI_SUCCESS) {
            g_info.mmap = (uint64_t)g_mmap;
            g_info.mmap_size = size;
            g_info.mmap_desc_size = desc_size;
            kernel_entry(&g_info);                          // UEFI n'existe plus : on passe au noyau
        }
    }
    for (;;) __asm__ volatile("wfe");                       // échec : on s'arrête
}
