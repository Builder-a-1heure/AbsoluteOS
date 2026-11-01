/* ==========================================================================
 * linker.ld — plan mémoire de l'image
 * L'image est liée à l'adresse 0 : toutes les adresses du fichier sont donc
 * des offsets relatifs à la base de chargement (choisie par le firmware UEFI).
 * boot.S corrige les pointeurs absolus au démarrage (voir "relocations").
 *
 * Deux sections PE distinctes (le firmware UEFI récent applique W^X) :
 *   .text : en-tête + code + constantes  -> lecture + exécution (jamais écrite)
 *   .data : variables, relocations, bss  -> lecture + écriture (jamais exécutée)
 * ========================================================================== */
ENTRY(_start)

SECTIONS
{
    . = 0;

    /* ---- Section PE ".text" : en-tête PE (0x0-0xFFF) puis code ---- */
    .text : {
        *(.text.boot)
        *(.text .text.*)
        *(.rodata .rodata.*)
    }
    __text_end = ABSOLUTE(.);

    /* ---- Section PE ".data" : démarre sur une page neuve (4 Ko) ---- */
    . = ALIGN(0x1000);
    __data_start = ABSOLUTE(.);

    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) }
    .got : { *(.got .got.plt) }
    .data : { *(.data .data.*) }

    /* Tables dynamiques générées par -pie ; seule .rela.dyn nous sert */
    .dynamic : { *(.dynamic) }
    .dynsym  : { *(.dynsym) }
    .dynstr  : { *(.dynstr) }
    .hash    : { *(.hash) *(.gnu.hash) }
    .rela.dyn : {
        __rela_start = .;
        *(.rela.dyn) *(.rela.*)
        __rela_end = .;
    }

    /* Remplissage jusqu'à une frontière de 4 Ko : fin des données stockées dans le fichier */
    .pad : { BYTE(0) . = ALIGN(0x1000); }
    __raw_end = ABSOLUTE(.);

    /* .bss : pas stockée dans le fichier, mise à zéro au démarrage */
    .bss : {
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(0x1000);
        __bss_end = .;
    }

    /* Valeurs réutilisées par l'en-tête PE (boot.S) */
    __text_vsize = ABSOLUTE(__text_end - 0x1000);        /* .text : taille en mémoire        */
    __text_rsize = ABSOLUTE(__data_start - 0x1000);      /* .text : taille dans le fichier   */
    __data_vsize = ABSOLUTE(__bss_end - __data_start);   /* .data : taille en mémoire (+bss) */
    __data_rsize = ABSOLUTE(__raw_end - __data_start);   /* .data : taille dans le fichier   */
    __image_size = ABSOLUTE(__bss_end);                  /* image entière en mémoire         */

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.interp) *(.gnu.*) }
}
