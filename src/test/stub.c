#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void kpanic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    abort();
}
