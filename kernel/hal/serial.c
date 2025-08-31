// LikeOS-64 HAL - 16550 UART Serial Console (COM1)
// Minimal polled driver for logging to QEMU -serial stdio

#include "../../include/kernel/serial.h"
#include "../../include/kernel/interrupt.h" // for inb/outb

#define COM1_PORT 0x3F8

// UART registers (offsets from base)
#define UART_DATA      0   // THR (write) / RBR (read)
#define UART_IER       1
#define UART_IIR_FCR   2   // IIR (read) / FCR (write)
#define UART_LCR       3
#define UART_MCR       4
#define UART_LSR       5
#define UART_MSR       6
#define UART_SCR       7

// LSR bits
#define LSR_DATA_READY   0x01
#define LSR_THR_EMPTY    0x20

static int g_serial_available = 0;
static int g_serial_initialized = 0;

static inline void uart_out(unsigned short off, unsigned char v)
{
    outb(COM1_PORT + off, v);
}

static inline unsigned char uart_in(unsigned short off)
{
    return inb(COM1_PORT + off);
}

static int uart_detect(void)
{
    /* Try SCR (scratch) register read-back test */
    unsigned char orig = uart_in(UART_SCR);
    uart_out(UART_SCR, 0x55);
    unsigned char t1 = uart_in(UART_SCR);
    uart_out(UART_SCR, 0xAA);
    unsigned char t2 = uart_in(UART_SCR);
    uart_out(UART_SCR, orig); /* Restore */
    return (t1 == 0x55 && t2 == 0xAA);
}

void serial_init(void)
{
    if(g_serial_initialized) {
        return;
    }

    // Disable interrupts
    uart_out(UART_IER, 0x00);

    // Enable DLAB to set divisor (baud rate). Divisor 1 -> 115200 baud
    uart_out(UART_LCR, 0x80);
    uart_out(UART_DATA, 0x01); // DLL
    uart_out(UART_IER, 0x00);  // DLM

    // 8 bits, no parity, one stop bit
    uart_out(UART_LCR, 0x03);

    // Enable FIFO, clear them, 14-byte threshold
    uart_out(UART_IIR_FCR, 0xC7);

    // Modem control: RTS/DTR set, OUT2 set (enables IRQ line; harmless here)
    uart_out(UART_MCR, 0x0B);

    g_serial_available = uart_detect();
    g_serial_initialized = 1;
}

int serial_is_available(void)
{
    return g_serial_available;
}

static inline void uart_wait_thr_empty(void)
{
    /* Busy-wait until THR empty */
    while((uart_in(UART_LSR) & LSR_THR_EMPTY) == 0) {
        /* spin */
    }
}

void serial_write_char(char c)
{
    if(!g_serial_initialized) {
        serial_init();
    }
    if(!g_serial_available) {
        return;
    }
    if(c == '\n') {
        /* Ensure CRLF on serial for better terminal behavior */
        uart_wait_thr_empty();
        uart_out(UART_DATA, '\r');
    }
    uart_wait_thr_empty();
    uart_out(UART_DATA, (unsigned char)c);
}

void serial_write(const char* s, uint32_t len)
{
    if(!g_serial_initialized) {
        serial_init();
    }
    if(!g_serial_available || !s) {
        return;
    }
    for(uint32_t i = 0; i < len; ++i) {
        serial_write_char(s[i]);
    }
}
