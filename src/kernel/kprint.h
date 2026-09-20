#pragma once

#include <stdarg.h>
#include <stdint.h>

void kvprint(const char *fmt, va_list ap);
void kprint(const char *fmt, ...);
int  ksnprintf(char *buf, uint32_t cap, const char *fmt, ...);
