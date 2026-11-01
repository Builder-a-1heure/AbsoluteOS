// ============================================================================
// input.h — Entrée unifiée : clavier USB + port série
// ============================================================================
#pragma once
#include "bootinfo.h"

int input_init(const BootInfo *bi);   // cherche un xHCI via ACPI/PCI ; renvoie le nombre de claviers USB prêts
int input_getc_nb(void);              // octet suivant (clavier d'abord, puis série) ou -1
