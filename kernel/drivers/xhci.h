// ============================================================================
// xhci.h — Contrôleur USB 3 (xHCI) + clavier HID "boot protocol"
// ============================================================================
#pragma once
#include <stdint.h>

int  xhci_init(uint64_t mmio_base);   // reset le contrôleur, énumère les ports ; renvoie le nombre de claviers prêts
void xhci_poll(void);                 // traite les événements (à appeler souvent : pas d'interruptions pour l'instant)
