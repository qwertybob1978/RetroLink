#include <conio.h>
#include <dos.h>

#include "dos_serial.h"

#define DOS_TICKS_PER_DAY 0x001800B0UL
#define DOS_TICKS_PER_SECOND 18UL
#define UART_LINE_STATUS 5
#define UART_DATA_READY 0x01
#define UART_TRANSMIT_EMPTY 0x20

rl_u32 dos_clock_ticks(void)
{
    unsigned long far *ticks;
    ticks = (unsigned long far *)MK_FP(0x40, 0x6C);
    return *ticks;
}

void dos_idle(void)
{
    /* Poll the UART continuously so its receive FIFO cannot overrun. */
}

int dos_uart_open(DosUart *uart, unsigned char port_number, rl_u32 baud_rate)
{
    unsigned short far *bios_ports;
    unsigned int divisor;

    if (uart == 0 || port_number < 1 || port_number > 4 ||
        baud_rate < 300 || baud_rate > 115200)
    {
        return 0;
    }

    bios_ports = (unsigned short far *)MK_FP(0x40, 0);
    uart->base = bios_ports[port_number - 1];
    if (uart->base == 0)
        return 0;
    divisor = (unsigned int)(115200UL / baud_rate);
    if (divisor == 0)
        return 0;

    uart->port_number = port_number;
    uart->baud_rate = baud_rate;
    outp(uart->base + 1, 0);
    outp(uart->base + 3, 0x80);
    outp(uart->base, divisor & 0xFF);
    outp(uart->base + 1, divisor >> 8);
    outp(uart->base + 3, 0x03);
    outp(uart->base + 2, 0x07);
    outp(uart->base + 4, 0x03);
    return 1;
}

int dos_uart_try_read(DosUart *uart, rl_u8 *value)
{
    if (uart == 0 || value == 0 || uart->base == 0)
        return 0;
    if ((inp(uart->base + UART_LINE_STATUS) & UART_DATA_READY) == 0)
        return 0;
    *value = (rl_u8)inp(uart->base);
    return 1;
}

int dos_uart_write(DosUart *uart, rl_u8 value, rl_u16 timeout_seconds)
{
    rl_u32 start;
    rl_u32 timeout_ticks;
    rl_u32 now;

    if (uart == 0 || uart->base == 0)
        return 0;
    start = dos_clock_ticks();
    timeout_ticks = (rl_u32)timeout_seconds * DOS_TICKS_PER_SECOND;
    do
    {
        if ((inp(uart->base + UART_LINE_STATUS) & UART_TRANSMIT_EMPTY) != 0)
        {
            outp(uart->base, value);
            return 1;
        }
        now = dos_clock_ticks();
        if (now < start)
        {
            if (DOS_TICKS_PER_DAY - start + now >= timeout_ticks)
                return 0;
        }
        else if (now - start >= timeout_ticks)
        {
            return 0;
        }
        dos_idle();
    } while (1);
}
