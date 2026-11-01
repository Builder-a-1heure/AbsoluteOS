// ============================================================================
// acpi.h — Lecture minimale des tables ACPI
// ============================================================================
#pragma once
#include <stdint.h>

// Cherche une table par sa signature de 4 lettres ("MCFG", "APIC", "FACP", ...).
// rsdp = adresse physique du RSDP (BootInfo.acpi_rsdp). Renvoie 0 si introuvable.
const uint8_t *acpi_find_table(uint64_t rsdp, const char *sig);

uint32_t acpi_rd32(const uint8_t *p);   // lecture little-endian octet par octet (les tables ne sont pas alignées)
uint64_t acpi_rd64(const uint8_t *p);
