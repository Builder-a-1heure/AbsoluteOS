// ============================================================================
// boot.S — Bootloader arm64 (assembleur)
//
// Ce fichier fait trois choses :
//   1. Il commence par un EN-TÊTE PE/COFF : c'est ce qui permet au firmware
//      UEFI (QEMU, PC ARM "SystemReady", serveurs...) de reconnaître ce
//      fichier comme une application et de le charger. Sur arm64 il n'y a
//      ni BIOS ni VESA : UEFI est la porte d'entrée commune des vraies machines.
//   2. _start : point d'entrée appelé par UEFI. Il se "relocalise", met la
//      .bss à zéro puis passe la main au C (efi_main, dans boot_efi.c).
//   3. kernel_entry : appelé une fois UEFI quitté. Il installe notre pile et
//      notre table d'exceptions puis lance kmain() (kernel.c).
//
// Rappel arm64 : registres x0..x30 (64 bits), sp = pile, xzr = registre zéro.
//   x0..x7  = arguments de fonction, x0 = valeur de retour, x30 = adresse de retour (lr).
//   Les registres x19..x28 sont préservés par les fonctions appelées : on s'en
//   sert pour garder des valeurs à travers un appel (bl).
// ============================================================================

    .section .text.boot, "ax"          // section exécutable, placée en premier par linker.ld

// ----------------------------------------------------------------------------
// PARTIE 1 — En-tête PE/COFF (occupe les 4096 premiers octets de l'image)
// ----------------------------------------------------------------------------
    .global __image_base
__image_base:                          // adresse 0 de l'image (début du fichier)

// ---- En-tête DOS (64 octets) : seul "MZ" et le pointeur vers l'en-tête PE comptent
    .ascii  "MZ"                       // signature DOS obligatoire
    .skip   58                         // champs DOS inutilisés (58 octets à zéro)
    .long   pe_header - __image_base   // e_lfanew : offset du "PE\0\0" dans le fichier

// ---- Signature + en-tête COFF (20 octets)
pe_header:
    .ascii  "PE\0\0"                   // signature PE
    .short  0xAA64                     // Machine : IMAGE_FILE_MACHINE_ARM64
    .short  2                          // NumberOfSections : .text (code) et .data (variables)
    .long   0                          // TimeDateStamp (inutile)
    .long   0                          // PointerToSymbolTable (aucune)
    .long   0                          // NumberOfSymbols
    .short  section_table - opt_header // SizeOfOptionalHeader (= 240 octets)
    .short  0x0206                     // Characteristics : exécutable, sans numéros de ligne

// ---- En-tête optionnel PE32+ (obligatoire malgré son nom)
opt_header:
    .short  0x20B                      // Magic : PE32+ (64 bits)
    .byte   0, 0                       // version du linker (inutile)
    .long   __text_rsize               // SizeOfCode : taille de la section .text dans le fichier
    .long   __data_rsize               // SizeOfInitializedData : taille de la section .data dans le fichier
    .long   0                          // SizeOfUninitializedData
    .long   _start - __image_base      // AddressOfEntryPoint : où UEFI saute (RVA de _start)
    .long   0x1000                     // BaseOfCode : le code commence après l'en-tête (4 Ko)
    .quad   0                          // ImageBase : adresse "préférée" (0, on se relocalise nous-mêmes)
    .long   0x1000                     // SectionAlignment : alignement en mémoire (4 Ko)
    .long   0x1000                     // FileAlignment : alignement dans le fichier (4 Ko)
    .short  0, 0                       // version de l'OS requise
    .short  0, 0                       // version de l'image
    .short  0, 0                       // version du sous-système
    .long   0                          // Win32VersionValue (réservé)
    .long   __image_size               // SizeOfImage : mémoire totale à réserver (en-tête+code+bss)
    .long   0x1000                     // SizeOfHeaders : taille de cet en-tête (4 Ko)
    .long   0                          // CheckSum (ignoré par UEFI)
    .short  10                         // Subsystem : 10 = EFI_APPLICATION
    .short  0x100                      // DllCharacteristics : NX_COMPAT (aucune section n'est à la fois écrivable et exécutable)
    .quad   0, 0, 0, 0                 // tailles pile/tas réservées/validées (inutilisées par UEFI)
    .long   0                          // LoaderFlags
    .long   16                         // NumberOfRvaAndSizes : 16 entrées de répertoire ci-dessous
    .skip   16 * 8                     // 16 répertoires de données vides (pas d'import, pas de .reloc)

// ---- Table des sections (2 entrées de 40 octets)
// Pourquoi deux sections : un firmware UEFI récent rend en lecture seule toute
// section exécutable (W^X). Si le code et les variables étaient ensemble, la
// première écriture (relocations plus bas) provoquerait une exception.
section_table:
    // -- Section 1 : code et constantes (lecture + exécution)
    .ascii  ".text\0\0\0"              // Name (8 octets)
    .long   __text_vsize               // VirtualSize : taille en mémoire
    .long   0x1000                     // VirtualAddress : chargée à base+0x1000 (juste après l'en-tête)
    .long   __text_rsize               // SizeOfRawData : taille dans le fichier
    .long   0x1000                     // PointerToRawData : offset dans le fichier
    .long   0, 0                       // pas de relocations / numéros de ligne COFF
    .short  0, 0
    .long   0x60000020                 // Characteristics : code | lisible | exécutable

    // -- Section 2 : variables, relocations et .bss (lecture + écriture)
    .ascii  ".data\0\0\0"              // Name
    .long   __data_vsize               // VirtualSize : taille en mémoire (.bss comprise)
    .long   __data_start               // VirtualAddress : page suivant la fin du code
    .long   __data_rsize               // SizeOfRawData : taille dans le fichier (sans .bss)
    .long   __data_start               // PointerToRawData : même valeur (fichier = image à plat)
    .long   0, 0
    .short  0, 0
    .long   0xC0000040                 // Characteristics : données initialisées | lisible | écrivable

    .balign 0x1000                     // remplit de zéros jusqu'à 4 Ko : le code démarre ici

// ----------------------------------------------------------------------------
// PARTIE 2 — _start : point d'entrée UEFI
//   Entrée : x0 = EFI_HANDLE de notre image, x1 = pointeur EFI_SYSTEM_TABLE
//   On est en 64 bits, MMU et caches activés par le firmware, pile fournie par UEFI.
// ----------------------------------------------------------------------------
    .global _start
_start:
    mov     x19, x0                    // sauvegarde ImageHandle (x19 survit aux appels de fonctions)
    mov     x20, x1                    // sauvegarde SystemTable

    // x21 = adresse réelle où le firmware nous a chargés ("base de chargement").
    // Comme l'image est liée à l'adresse 0, une adresse réelle = base + adresse liée.
    adrp    x21, __image_base          // adrp = adresse de la page 4 Ko contenant le symbole (relatif au PC)
    add     x21, x21, :lo12:__image_base // + les 12 bits bas (0 ici) => x21 = base de chargement

    // ---- Relocations : corrige les pointeurs absolus stockés dans nos données
    // (ex. tables de pointeurs de fonctions). Le linker les liste dans .rela.dyn,
    // chaque entrée fait 24 octets : { r_offset, r_info, r_addend }.
    // Pour le type R_AARCH64_RELATIVE : *(base + r_offset) = base + r_addend.
    adrp    x0, __rela_start
    add     x0, x0, :lo12:__rela_start // x0 = début de la table de relocations (adresse réelle)
    adrp    x1, __rela_end
    add     x1, x1, :lo12:__rela_end   // x1 = fin de la table
1:  cmp     x0, x1                     // a-t-on parcouru toute la table ?
    b.hs    2f                         // oui (x0 >= x1) -> on sort de la boucle
    ldp     x2, x3, [x0]               // x2 = r_offset, x3 = r_info (ldp charge 2 registres d'un coup)
    ldr     x4, [x0, #16]              // x4 = r_addend
    cmp     w3, #1027                  // type 1027 = R_AARCH64_RELATIVE ? (seul type produit ici)
    b.ne    3f                         // autre type : on l'ignore
    add     x4, x4, x21                // valeur finale = base + addend
    str     x4, [x21, x2]              // écrit la valeur à l'adresse base + r_offset
3:  add     x0, x0, #24                // entrée suivante
    b       1b                         // boucle
2:
    // ---- Met la .bss à zéro (variables globales non initialisées doivent valoir 0)
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start  // x0 = début de .bss
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end    // x1 = fin de .bss
4:  cmp     x0, x1                     // fini ?
    b.hs    5f
    str     xzr, [x0], #8              // écrit 8 octets à zéro puis avance x0 de 8 (post-incrément)
    b       4b
5:
    mov     x0, x19                    // 1er argument : ImageHandle
    mov     x1, x20                    // 2e argument : SystemTable
    bl      efi_main                   // C : récupère écran + mémoire, quitte UEFI, appelle kernel_entry
                                       // (ne revient jamais ; si ça revient, on s'arrête ci-dessous)
    b       halt

// ----------------------------------------------------------------------------
// halt : arrête le cœur courant pour toujours (wfe = attend un événement)
// ----------------------------------------------------------------------------
    .global halt
halt:
    wfe                                // met le cœur en veille basse consommation
    b       halt                       // si jamais il se réveille, on se rendort

// ----------------------------------------------------------------------------
// PARTIE 3 — kernel_entry(BootInfo *info)
//   Appelé par efi_main APRÈS ExitBootServices : UEFI n'existe plus.
//   Entrée : x0 = pointeur BootInfo (écran, carte mémoire, ACPI)
// ----------------------------------------------------------------------------
    .global kernel_entry
kernel_entry:
    // ---- Pile du noyau : celle d'UEFI est dans de la mémoire qu'on va récupérer
    adrp    x1, kstack_top
    add     x1, x1, :lo12:kstack_top   // x1 = haut de notre pile (la pile arm64 grandit vers le bas)
    mov     sp, x1                     // sp = notre pile à nous (alignée sur 16 octets, obligatoire)

    // ---- Table des vecteurs d'exceptions : où le CPU saute en cas d'erreur/IRQ
    adrp    x1, vectors
    add     x1, x1, :lo12:vectors      // x1 = adresse de la table (alignée sur 2 Ko)
    mrs     x2, CurrentEL              // lit le niveau d'exception courant (bits 3:2)
    lsr     x2, x2, #2                 // x2 = 1 (EL1, noyau) ou 2 (EL2, hyperviseur)
    cmp     x2, #2
    b.eq    1f                         // on est en EL2 ?
    msr     vbar_el1, x1               // EL1 : installe la table dans VBAR_EL1
    b       2f
1:  msr     vbar_el2, x1               // EL2 : installe la table dans VBAR_EL2
2:  isb                                // barrière : attend que l'écriture soit effective

    bl      kmain                      // C : le noyau (x0 = BootInfo déjà en place)
    b       halt                       // kmain ne doit pas revenir

// ----------------------------------------------------------------------------
// Table des vecteurs d'exceptions : 16 entrées de 128 octets (0x80), alignée sur 0x800.
//   4 groupes (EL courant+SP0, EL courant+SPx, EL inférieur AArch64, EL inférieur AArch32)
//   x 4 types (Synchrone, IRQ, FIQ, SError).
//   Chaque entrée charge son numéro (0..15) dans x0 puis saute au gestionnaire commun.
//   Une faute dans NOTRE code (accès invalide, instruction illégale) arrive à l'entrée 4.
// ----------------------------------------------------------------------------
    .macro vector_entry n
    .balign 0x80                       // chaque entrée occupe exactement 128 octets
    mov     x0, #\n                    // x0 = numéro de l'entrée (type d'exception)
    b       exception_common           // gestionnaire commun
    .endm

    .balign 0x800                      // la table entière doit être alignée sur 2 Ko
vectors:
    vector_entry 0
    vector_entry 1
    vector_entry 2
    vector_entry 3
    vector_entry 4
    vector_entry 5
    vector_entry 6
    vector_entry 7
    vector_entry 8
    vector_entry 9
    vector_entry 10
    vector_entry 11
    vector_entry 12
    vector_entry 13
    vector_entry 14
    vector_entry 15

// ----------------------------------------------------------------------------
// exception_common : lit les registres de diagnostic et appelle le C (affiche + stoppe)
//   ESR = cause de l'exception, ELR = adresse de l'instruction fautive,
//   FAR = adresse mémoire accédée (si c'est un accès invalide)
// ----------------------------------------------------------------------------
exception_common:
    mrs     x9, CurrentEL
    lsr     x9, x9, #2                 // x9 = EL courant (1 ou 2)
    cmp     x9, #2
    b.eq    1f
    mrs     x1, esr_el1                // EL1 : 2e argument = ESR
    mrs     x2, elr_el1                //        3e argument = ELR
    mrs     x3, far_el1                //        4e argument = FAR
    b       2f
1:  mrs     x1, esr_el2                // EL2 : mêmes registres, version EL2
    mrs     x2, elr_el2
    mrs     x3, far_el2
2:  bl      exception_report           // C : exception_report(n, esr, elr, far) — ne revient pas
    b       halt

// ----------------------------------------------------------------------------
// Pile du noyau : 64 Ko réservés en .bss (donc mis à zéro, pas stockés dans le fichier)
// ----------------------------------------------------------------------------
    .section .bss
    .balign 16                         // la pile arm64 doit être alignée sur 16 octets
kstack_bottom:
    .skip   64 * 1024
    .global kstack_top
kstack_top:
