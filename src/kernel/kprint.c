#include <stdarg.h>
#include <stdint.h>

#include "kprint.h"
#include "uart.h"

/* emit target: UART by default, a caller buffer while ksnprintf runs */
static char    *sn_buf;
static uint32_t sn_cap, sn_len;

static void emit(char c)
{
    if (sn_buf) {
        if (sn_len + 1 < sn_cap)
            sn_buf[sn_len] = c;
        sn_len++;
    } else {
        uart_putc(c);
    }
}

static void emit_str(const char *s)
{
    while (*s)
        emit(*s++);
}

static void put_u64(uint64_t v, unsigned base, int upper, int width, int pad0)
{
    char buf[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;

    if (v == 0)
        buf[i++] = '0';
    while (v) {
        buf[i++] = digits[v % base];
        v /= base;
    }
    while (width-- > i)
        emit(pad0 ? '0' : ' ');
    while (i--)
        emit(buf[i]);
}

static void put_i64(int64_t v, int width, int pad0)
{
    uint64_t u = (uint64_t)v;
    if (v < 0) {
        emit('-');
        u = (uint64_t)(-(v + 1)) + 1;
    }
    put_u64(u, 10, 0, width, pad0);
}

void kvprint(const char *fmt, va_list ap)
{
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            emit(*p);
            continue;
        }
        p++;
        int pad0 = 0, width = 0;
        if (*p == '0') {
            pad0 = 1;
            p++;
        }
        while (*p >= '0' && *p <= '9')
            width = width * 10 + (*p++ - '0');
        int lng = 0;
        if (*p == 'l') {
            lng = 1;
            p++;
        }
        switch (*p) {
        case '%':
            emit('%');
            break;
        case 'c':
            emit((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            emit_str(s ? s : "(null)");
            break;
        }
        case 'd':
        case 'i':
            put_i64(lng ? va_arg(ap, long) : va_arg(ap, int), width, pad0);
            break;
        case 'u':
            put_u64(lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned),
                    10, 0, width, pad0);
            break;
        case 'x':
            put_u64(lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned),
                    16, 0, width, pad0);
            break;
        case 'X':
            put_u64(lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned),
                    16, 1, width, pad0);
            break;
        case 'p':
            emit_str("0x");
            put_u64((uint64_t)va_arg(ap, void *), 16, 0, 0, 0);
            break;
        default:
            emit('%');
            emit(*p);
            break;
        }
    }
}

void kprint(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprint(fmt, ap);
    va_end(ap);
}

int ksnprintf(char *buf, uint32_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sn_buf = buf;
    sn_cap = cap;
    sn_len = 0;
    kvprint(fmt, ap);
    if (buf && cap)
        buf[sn_len < cap ? sn_len : cap - 1] = 0;
    sn_buf = 0;
    va_end(ap);
    return (int)sn_len;                 /* would-be length, snprintf-style */
}
