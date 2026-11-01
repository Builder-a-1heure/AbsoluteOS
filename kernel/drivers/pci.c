// ============================================================================
// pci.c — Énumération PCIe minimale
// L'adresse ECAM vient de la table ACPI MCFG. Le firmware a déjà attribué les
// BAR (adresses des registres de chaque périphérique) : on se contente de les lire.
// ============================================================================
#include "pci.h"
#include "acpi.h"

static uint64_t g_ecam;                // adresse de la configuration du bus g_bus_start
static uint8_t  g_bus_start, g_bus_end;

int pci_init(const uint8_t *mcfg) {
    if (!mcfg) return 0;
    // MCFG : en-tête ACPI (36) + 8 octets réservés, puis des entrées de 16 octets :
    //   base (8), segment (2), bus début (1), bus fin (1), réservé (4)
    if (acpi_rd32(mcfg + 4) < 44 + 16) return 0;
    g_ecam = acpi_rd64(mcfg + 44);
    g_bus_start = mcfg[44 + 10];
    g_bus_end   = mcfg[44 + 11];
    return g_ecam != 0;
}

// Adresse des 4 Ko de configuration d'une fonction (bus, périphérique, fonction)
static volatile uint32_t *cfg_of(uint32_t bus, uint32_t dev, uint32_t fn) {
    return (volatile uint32_t *)(g_ecam + (((uint64_t)(bus - g_bus_start) << 20) | (dev << 15) | (fn << 12)));
}

volatile uint32_t *pci_find_class(uint8_t base, uint8_t sub, uint8_t prog) {
    uint32_t want = ((uint32_t)base << 16) | ((uint32_t)sub << 8) | prog;
    for (uint32_t bus = g_bus_start; bus <= g_bus_end; bus++)
        for (uint32_t dev = 0; dev < 32; dev++)
            for (uint32_t fn = 0; fn < 8; fn++) {
                volatile uint32_t *c = cfg_of(bus, dev, fn);
                if ((c[0] & 0xFFFF) == 0xFFFF) { if (fn == 0) break; else continue; }  // rien ici
                if ((c[2] >> 8) == want) return c;                                      // classe trouvée
                if (fn == 0 && !((c[3] >> 16) & 0x80)) break;   // pas multi-fonction : inutile d'essayer fn 1..7
            }
    return 0;
}

void pci_enable(volatile uint32_t *cfg) {
    volatile uint16_t *command = (volatile uint16_t *)((volatile uint8_t *)cfg + 4);
    *command = *command | 0x6;         // bit 1 = mémoire, bit 2 = bus master
}
