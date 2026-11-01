// ============================================================================
// uart.c — Driver PL011
//
// ATTENTION : 0x09000000 est l'adresse de la machine virtuelle QEMU "virt".
// Sur une vraie machine elle vient de l'ACPI (table SPCR) : prévu à l'étape ACPI.
// ============================================================================
#include <stdint.h>
#include "uart.h"

#define UART_BASE     0x09000000UL
#define UART_DR       (*(volatile uint32_t *)(UART_BASE + 0x00))  // données (lecture = reçu, écriture = émis)
#define UART_FR       (*(volatile uint32_t *)(UART_BASE + 0x18))  // statut
#define UART_CR       (*(volatile uint32_t *)(UART_BASE + 0x30))  // contrôle

#define UART_FR_RXFE  (1u << 4)    // file de réception vide
#define UART_FR_TXFF  (1u << 5)    // file d'émission pleine
#define UART_CR_UARTEN (1u << 0)   // UART activée
#define UART_CR_TXE    (1u << 8)   // émission activée
#define UART_CR_RXE    (1u << 9)   // réception activée

// Le firmware a déjà configuré la vitesse ; on s'assure juste que TX et RX sont actifs
void uart_init(void) {
    UART_CR |= UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;
}

// Envoie un caractère sur le port série
void uart_putc(char c) {
    if (c == '\n') uart_putc('\r');         // les terminaux attendent \r\n
    while (UART_FR & UART_FR_TXFF) {}       // attend qu'il y ait de la place
    UART_DR = (uint32_t)(uint8_t)c;         // écrire dans DR envoie l'octet
}

// Lit un octet s'il y en a un, sinon renvoie -1 (jamais bloquant)
int uart_getc_nb(void) {
    if (UART_FR & UART_FR_RXFE) return -1;
    return (int)(UART_DR & 0xFF);
}
