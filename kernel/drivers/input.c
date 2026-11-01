// ============================================================================
// input.c — Trouve le contrôleur USB et fournit une seule source d'entrée
// Chaîne : ACPI (table MCFG) -> PCIe (ECAM) -> contrôleur xHCI (classe 0C/03/30) -> clavier
// ============================================================================
#include "input.h"
#include "acpi.h"
#include "pci.h"
#include "xhci.h"
#include "keyboard.h"
#include "uart.h"
#include "kprintf.h"

int input_init(const BootInfo *bi) {
    const uint8_t *mcfg = acpi_find_table(bi->acpi_rsdp, "MCFG");
    if (!pci_init(mcfg)) { kputs("Clavier USB : pas de PCIe (table MCFG absente)\n"); return 0; }
    volatile uint32_t *cfg = pci_find_class(0x0C, 0x03, 0x30);     // 0C = serial bus, 03 = USB, 30 = xHCI
    if (!cfg) { kputs("Clavier USB : aucun contrôleur xHCI\n"); return 0; }
    uint32_t bar0 = cfg[4];                                         // BAR0 : adresse des registres du contrôleur
    uint64_t base = bar0 & ~0xFu;
    if ((bar0 & 6) == 4) base |= (uint64_t)cfg[5] << 32;            // BAR 64 bits : moitié haute dans BAR1
    if (!base) { kputs("Clavier USB : BAR xHCI non attribue\n"); return 0; }
    pci_enable(cfg);
    return xhci_init(base);
}

int input_getc_nb(void) {
    xhci_poll();                          // traite les événements USB (remplit la file du clavier)
    int c = kbd_getc_nb();
    if (c >= 0) return c;
    return uart_getc_nb();                // sinon le port série
}
