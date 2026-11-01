// ============================================================================
// main.c — Point d'entrée du noyau (appelé par kernel_entry dans boot.S)
// ============================================================================
#include <stdint.h>
#include "bootinfo.h"
#include "uart.h"
#include "console.h"
#include "kprintf.h"
#include "logo.h"
#include "meminfo.h"
#include "input.h"
#include "ramdisk.h"
#include "ray64.h"
#include "shell.h"

// Rapport d'exception : appelé par exception_common (boot.S), ne revient pas
//   n = numéro d'entrée de la table (4 = faute synchrone dans le noyau)
//   ESR = cause, ELR = instruction fautive, FAR = adresse accédée
void exception_report(uint64_t n, uint64_t esr, uint64_t elr, uint64_t far) {
    kprintf("\n*** EXCEPTION ***\n  entree=%lu\n  ESR=%016lx\n  ELR=%016lx\n  FAR=%016lx\n", n, esr, elr, far);
    for (;;) __asm__ volatile("wfe");
}

// kmain : initialise les pilotes puis lance le terminal — ne revient pas
void kmain(BootInfo *bi) {
    uart_init();
    mem_init(bi);
    console_init(bi);                                     // si pas d'écran : on reste sur le port série

    console_set_color(CON_LGRAY, CON_BLACK);
    logo_show();                                          // logo tout en haut de l'écran
    if (console_ready())
        kprintf("Ecran : %ux%u, console %u x %u\n", bi->fb_width, bi->fb_height, console_cols(), console_rows());
    else
        kputs("Ecran : indisponible (serie seulement)\n");
    kprintf("RAM utilisable : %lu Mo (%u zones)\n", mem_usable_bytes() >> 20, mem_region_count());
    kprintf("ACPI RSDP : %p\n", (void *)bi->acpi_rsdp);

    int kbds = input_init(bi);                            // clavier USB (ACPI -> PCIe -> xHCI)
    if (kbds > 0) kprintf("Clavier USB : %d detecte(s)\n", kbds);
    else          kputs("Clavier USB : aucun (entree par le port serie)\n");

    // Volume Ray64 : sur le disque RAM (vide à chaque démarrage tant qu'il n'y a pas de pilote de disque réel)
    if (ray64_mount(ramdisk_dev()) == R64_OK) kputs("Volume Ray64 : monte\n");
    else                                      kputs("Volume Ray64 : aucun (tape : fs format)\n");

    shell_run();
}
