// ============================================================================
// uart.h — Port série PL011 (sortie texte + entrée clavier du terminal)
// ============================================================================
#pragma once

void uart_init(void);          // active l'UART (émission + réception)
void uart_putc(char c);        // envoie un caractère ('\n' devient "\r\n")
int  uart_getc_nb(void);       // lit un octet sans attendre : 0..255, ou -1 si rien reçu
