#include <stdint.h>

#include "exception.h"
#include "frame.h"
#include "kprint.h"
#include "panic.h"
#include "uart.h"

static int in_panic;

static const char *ec_name(uint32_t ec)
{
    switch (ec) {
    case 0x00: return "unknown";
    case 0x01: return "wfi trap";
    case 0x07: return "fp/simd access";
    case 0x15: return "svc (aarch64)";
    case 0x18: return "sysreg trap";
    case 0x20: return "insn abort (lower el)";
    case 0x21: return "insn abort (same el)";
    case 0x22: return "pc alignment";
    case 0x24: return "data abort (lower el)";
    case 0x25: return "data abort (same el)";
    case 0x26: return "sp alignment";
    case 0x2f: return "serror";
    case 0x30: return "breakpoint (lower el)";
    case 0x31: return "breakpoint (same el)";
    case 0x32: return "step (lower el)";
    case 0x33: return "step (same el)";
    case 0x34: return "watchpoint (lower el)";
    case 0x35: return "watchpoint (same el)";
    case 0x3c: return "brk (aarch64)";
    default:   return "other";
    }
}

static void dump_frame(const struct trap_frame *f)
{
    kprint("    elr=%016lx spsr=%08lx\n", f->elr, f->spsr);
    for (int i = 0; i < 28; i += 4)
        kprint("    x%02d=%016lx x%02d=%016lx x%02d=%016lx x%02d=%016lx\n",
               i, f->x[i], i + 1, f->x[i + 1], i + 2, f->x[i + 2],
               i + 3, f->x[i + 3]);
    kprint("    x28=%016lx x29=%016lx x30=%016lx\n",
           f->x[28], f->x[29], f->x[30]);
}

void sync_exception(struct trap_frame *frame)
{
    if (in_panic++ != 0 || !uart_ready)
        hcf();

    uint64_t esr, far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    uint32_t ec = (esr >> 26) & 0x3f;
    kprint("\n*** sync exception: %s (ec=0x%02x)\n", ec_name(ec), ec);
    kprint("    esr=%08lx far=%016lx\n", esr, far);
    dump_frame(frame);
    hcf();
}

void fatal_exception(struct trap_frame *frame, int kind)
{
    if (in_panic++ != 0 || !uart_ready)
        hcf();

    static const char *names[] = { "?", "fiq", "serror", "lower-el sync" };
    kprint("\n*** fatal exception: %s\n", names[kind & 3]);
    dump_frame(frame);
    hcf();
}
