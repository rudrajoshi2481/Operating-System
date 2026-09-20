#pragma once

#include <stdint.h>

/* virtio-console (port 0) on the shared mmio transport: guest <-> host
 * byte stream. The host end is whatever chardev QEMU is given (unix
 * socket in our Makefile). RX is IRQ-driven into a ring buffer. */
int  con_init(uint64_t hhdm);
int  con_write(const void *buf, uint32_t len);  /* 0 = ok */
int  con_read(void *buf, uint32_t len);         /* bytes read (0 = empty) */
void con_wait(void);                            /* sleep until RX data */
