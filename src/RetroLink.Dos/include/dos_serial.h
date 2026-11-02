#ifndef RETROLINK_DOS_SERIAL_H
#define RETROLINK_DOS_SERIAL_H

#include "wire.h"

typedef struct
{
    unsigned int base;
    rl_u32 baud_rate;
    unsigned char port_number;
} DosUart;

int dos_uart_open(DosUart *uart, unsigned char port_number, rl_u32 baud_rate);
int dos_uart_try_read(DosUart *uart, rl_u8 *value);
int dos_uart_write(DosUart *uart, rl_u8 value, rl_u16 timeout_seconds);
rl_u32 dos_clock_ticks(void);
void dos_idle(void);

#endif
