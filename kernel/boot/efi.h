// ============================================================================
// efi.h — Définitions UEFI minimales (écrites à la main, aucune dépendance)
// On ne déclare que ce qui sert ; les champs inutilisés sont des "void *" pour
// garder les bons décalages (offsets) dans les structures.
// ============================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint64_t EFI_STATUS;
typedef void    *EFI_HANDLE;
#define EFIAPI                       // sur arm64, UEFI utilise la convention d'appel C standard
#define EFI_SUCCESS            0ULL
#define EFI_BUFFER_TOO_SMALL   (0x8000000000000005ULL)   // bit 63 = "erreur"

typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } EFI_GUID;

// En-tête commun à toutes les tables UEFI
typedef struct { uint64_t sig; uint32_t rev, size, crc, reserved; } EFI_TABLE_HEADER;

// Un descripteur de la carte mémoire. ATTENTION : on avance de "desc_size" octets
// (donné par le firmware) et non de sizeof(EFI_MEMORY_DESCRIPTOR).
typedef struct {
    uint32_t type;           // voir EFI_MEM_* ci-dessous
    uint32_t pad;
    uint64_t phys_start;     // adresse physique de début
    uint64_t virt_start;     // (inutilisé)
    uint64_t pages;          // nombre de pages de 4 Ko
    uint64_t attribute;
} EFI_MEMORY_DESCRIPTOR;

// Types de mémoire UEFI utiles
#define EFI_MEM_LOADER_CODE    1
#define EFI_MEM_LOADER_DATA    2
#define EFI_MEM_BS_CODE        3   // code des services de boot (libre après ExitBootServices)
#define EFI_MEM_BS_DATA        4   // données des services de boot (libre après ExitBootServices)
#define EFI_MEM_RT_CODE        5   // services runtime : à NE PAS toucher
#define EFI_MEM_RT_DATA        6
#define EFI_MEM_CONVENTIONAL   7   // RAM libre
#define EFI_MEM_ACPI_RECLAIM   9

// Table des services de boot : l'ordre des champs DOIT suivre la spec UEFI
typedef struct {
    EFI_TABLE_HEADER hdr;
    void *RaiseTPL, *RestoreTPL, *AllocatePages, *FreePages;
    EFI_STATUS (EFIAPI *GetMemoryMap)(uint64_t *size, EFI_MEMORY_DESCRIPTOR *map,
                                      uint64_t *key, uint64_t *desc_size, uint32_t *desc_ver);
    void *AllocatePool, *FreePool, *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent,
         *CloseEvent, *CheckEvent, *InstallProtocolInterface, *ReinstallProtocolInterface,
         *UninstallProtocolInterface, *HandleProtocol, *Reserved, *RegisterProtocolNotify,
         *LocateHandle, *LocateDevicePath, *InstallConfigurationTable, *LoadImage,
         *StartImage, *Exit, *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE image, uint64_t map_key);
    void *GetNextMonotonicCount, *Stall, *SetWatchdogTimer, *ConnectController,
         *DisconnectController, *OpenProtocol, *CloseProtocol, *OpenProtocolInformation,
         *ProtocolsPerHandle, *LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(const EFI_GUID *proto, void *registration, void **iface);
} EFI_BOOT_SERVICES;

typedef struct { EFI_GUID guid; void *table; } EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER hdr;
    void *FirmwareVendor;
    uint32_t FirmwareRevision, pad;
    void *ConsoleInHandle, *ConIn, *ConsoleOutHandle, *ConOut, *StandardErrorHandle, *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    uint64_t NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

// ---- Graphics Output Protocol (GOP) : l'équivalent moderne de VESA/VBE ----
typedef struct {
    uint32_t version, width, height;
    uint32_t pixel_format;           // 0 = RGBX, 1 = BGRX (8 bits/composante), 2 = masque, 3 = Blt seulement
    uint32_t masks[4];
    uint32_t pixels_per_scanline;    // largeur réelle d'une ligne en pixels (>= width)
} EFI_GOP_MODE_INFO;

typedef struct {
    uint32_t max_mode, mode;
    EFI_GOP_MODE_INFO *info;
    uint64_t info_size;
    uint64_t fb_base;                // adresse physique du framebuffer
    uint64_t fb_size;
} EFI_GOP_MODE;

typedef struct { void *QueryMode, *SetMode, *Blt; EFI_GOP_MODE *mode; } EFI_GOP;

// GUID du GOP et de la table ACPI 2.0 (pour retrouver le RSDP)
#define EFI_GOP_GUID  {0x9042a9de, 0x23dc, 0x4a38, {0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}}
#define EFI_ACPI20_GUID {0x8868e871, 0xe4f1, 0x11d3, {0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}}
