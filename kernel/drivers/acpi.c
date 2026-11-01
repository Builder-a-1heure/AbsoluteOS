// ============================================================================
// acpi.c — RSDP -> XSDT (ou RSDT) -> table recherchée
// ============================================================================
#include "acpi.h"

uint32_t acpi_rd32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t acpi_rd64(const uint8_t *p) {
    return acpi_rd32(p) | ((uint64_t)acpi_rd32(p + 4) << 32);
}

// Compare les 4 premiers octets de la table avec la signature demandée
static int sig_is(const uint8_t *t, const char *sig) {
    for (int i = 0; i < 4; i++) if (t[i] != (uint8_t)sig[i]) return 0;
    return 1;
}

const uint8_t *acpi_find_table(uint64_t rsdp, const char *sig) {
    const uint8_t *r = (const uint8_t *)rsdp;
    if (!r) return 0;
    const char *magic = "RSD PTR ";
    for (int i = 0; i < 8; i++) if (r[i] != (uint8_t)magic[i]) return 0;   // pas un RSDP valide

    // Révision >= 2 : XSDT (pointeurs 64 bits, offset 24 du RSDP) ; sinon RSDT (32 bits, offset 16)
    uint64_t xsdt = (r[15] >= 2) ? acpi_rd64(r + 24) : 0;
    const uint8_t *root = (const uint8_t *)(xsdt ? xsdt : (uint64_t)acpi_rd32(r + 16));
    if (!root) return 0;
    uint32_t entry_size = xsdt ? 8 : 4;
    uint32_t len = acpi_rd32(root + 4);                       // longueur totale de la table racine
    for (uint32_t off = 36; off + entry_size <= len; off += entry_size) {   // 36 = en-tête ACPI standard
        uint64_t addr = xsdt ? acpi_rd64(root + off) : acpi_rd32(root + off);
        const uint8_t *t = (const uint8_t *)addr;
        if (t && sig_is(t, sig)) return t;
    }
    return 0;
}
