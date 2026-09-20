#include <stdarg.h>
#include <stdint.h>

#include "kprint.h"
#include "uart.h"

static void put_u64(uint64_t v, unsigned base, int upper)
{
    char buf[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;

    if (v == 0) {
        uart_putc('0');
        return;
    }
    while (v) {
        buf[i++] = digits[v % base];
        v /= base;
    }
    while (i--)
        uart_putc(buf[i]);
}

static void put_i64(int64_t v)
{
    uint64_t u = (uint64_t)v;
    if (v < 0) {
        uart_putc('-');
        u = (uint64_t)(-(v + 1)) + 1;
    }
    put_u64(u, 10, 0);
}

void kprint(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            uart_putc(*p);
            continue;
        }
        p++;
        int lng = 0;
        if (*p == 'l') {
            lng = 1;
            p++;
        }
        switch (*p) {
        case '%':
            uart_putc('%');
            break;
        case 'c':
            uart_putc((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            uart_write(s ? s : "(null)");
            break;
        }
        case 'd':
        case 'i':
            put_i64(lng ? va_arg(ap, long) : va_arg(ap, int));
            break;
        case 'u':
            put_u64(lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 10, 0);
            break;
        case 'x':
            put_u64(lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, 0);
            break;
        case 'X':
            put_u64(lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, 1);
            break;
        case 'p':
            uart_write("0x");
            put_u64((uint64_t)va_arg(ap, void *), 16, 0);
            break;
        default:
            uart_putc('%');
            uart_putc(*p);
            break;
        }
    }

    va_end(ap);
}
