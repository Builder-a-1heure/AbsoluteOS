// ============================================================================
// pci.h — Accès PCIe via ECAM (configuration mappée en mémoire)
// ============================================================================
#pragma once
#include <stdint.h>

int pci_init(const uint8_t *mcfg);                                    // 1 si la table MCFG est exploitable
volatile uint32_t *pci_find_class(uint8_t base, uint8_t sub, uint8_t prog);  // 1er périphérique de cette classe (ou 0)
void pci_enable(volatile uint32_t *cfg);                              // active accès mémoire + bus master (DMA)
