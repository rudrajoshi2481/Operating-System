/*
 * idt.c — x86_64 IDT: exceptions 0-31 + PIC IRQs 32-47.
 * Exceptions dump the frame and halt; IRQ0 (PIT) drives the scheduler.
 */
#include "idt.h"
#include "io.h"
#include "kprint.h"
#include "panic.h"
#include "thread.h"
#include "timer.h"

struct idt_entry {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  attr;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t rsv;
} __attribute__((packed));

static struct idt_entry idt[256];

struct irq_frame_x86 {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vec, err, rip, cs, rflags, rsp, ss;
};

extern void isr0(void);  extern void isr8(void);  extern void isr10(void);
extern void isr11(void); extern void isr12(void); extern void isr13(void);
extern void isr14(void); extern void isr17(void); extern void isr21(void);
extern void isr29(void); extern void isr30(void);

#define DECL(n) extern void isr##n(void)
DECL(1);  DECL(2);  DECL(3);  DECL(4);  DECL(5);  DECL(6);  DECL(7);
DECL(9);  DECL(15); DECL(16); DECL(18); DECL(19); DECL(20); DECL(22);
DECL(23); DECL(24); DECL(25); DECL(26); DECL(27); DECL(28); DECL(31);
DECL(32); DECL(33); DECL(34); DECL(35); DECL(36); DECL(37); DECL(38);
DECL(39); DECL(40); DECL(41); DECL(42); DECL(43); DECL(44); DECL(45);
DECL(46); DECL(47);

static void gate(int vec, void *fn)
{
    uint64_t a = (uint64_t)fn;
    uint64_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));   /* limine's own kcode */
    idt[vec] = (struct idt_entry){
        .off_lo  = a & 0xffff,
        .sel     = cs,
        .ist     = 0,
        .attr    = 0x8e,                 /* present, ring0, int gate */
        .off_mid = (a >> 16) & 0xffff,
        .off_hi  = (uint32_t)(a >> 32),
    };
}

void exc_handler(struct irq_frame_x86 *f)
{
    kprint("\nEXC vec=%lu err=%lx rip=%lx\n", f->vec, f->err, f->rip);
    kprint("  rax=%lx rbx=%lx rcx=%lx rdx=%lx\n",
           f->rax, f->rbx, f->rcx, f->rdx);
    kprint("  rsi=%lx rdi=%lx rbp=%lx rsp=%lx\n",
           f->rsi, f->rdi, f->rbp, f->rsp);
    hcf();
}

/* PIC remap to 32/40, unmask cascade+IRQ0 (PIT) only. */
static void pic_init(void)
{
    /* UEFI leaves the LAPIC enabled; while it is, the PIC's INTR line
     * is gated by LINT0 (usually masked). Disable the LAPIC entirely
     * (clear x2APIC + global enable) so the 8259 drives the CPU. */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1b));
    __asm__ volatile("wrmsr" :: "a"(lo & ~0xc00u), "d"(hi), "c"(0x1b));

    outb(0x20, 0x11);                   /* ICW1: init, need ICW4 */
    outb(0xa0, 0x11);
    outb(0x21, 32);                     /* ICW2: master base vec */
    outb(0xa1, 40);                     /* slave base vec */
    outb(0x21, 4);                      /* ICW3: slave on IRQ2 */
    outb(0xa1, 2);
    outb(0x21, 1);                      /* ICW4: 8086 mode */
    outb(0xa1, 1);
    outb(0x21, 0xfe);                   /* mask all but IRQ0 */
    outb(0xa1, 0xff);
}

static void pic_eoi(uint32_t vec)
{
    if (vec >= 40)
        outb(0xa0, 0x20);
    outb(0x20, 0x20);
}

static irq_fn handlers[256];

void irq_register(uint32_t vec, irq_fn fn)
{
    handlers[vec] = fn;
}

void irq_dispatch(void *vf)
{
    struct irq_frame_x86 *f = vf;
    uint32_t vec = (uint32_t)f->vec;
    pic_eoi(vec);                       /* EOI first: a tick may swtch */
    if (vec == 32) {
        timer_tick_irq();               /* bumps ticks + sched_tick */
    } else if (handlers[vec]) {
        handlers[vec]((struct trap_frame *)f);
    }
}

/* sti before the IDT is loaded would triple-fault on any IRQ, so
 * irq_unmask() is a no-op until x86_irq_init() has run. */
static int irqs_live;

void irq_unmask(void)
{
    if (irqs_live)
        __asm__ volatile("sti");
}
void irq_mask(void)   { __asm__ volatile("cli"); }

void x86_irq_init(void)
{
    gate(0, isr0);   gate(1, isr1);   gate(2, isr2);   gate(3, isr3);
    gate(4, isr4);   gate(5, isr5);   gate(6, isr6);   gate(7, isr7);
    gate(8, isr8);   gate(9, isr9);   gate(10, isr10); gate(11, isr11);
    gate(12, isr12); gate(13, isr13); gate(14, isr14); gate(15, isr15);
    gate(16, isr16); gate(17, isr17); gate(18, isr18); gate(19, isr19);
    gate(20, isr20); gate(21, isr21); gate(22, isr22); gate(23, isr23);
    gate(24, isr24); gate(25, isr25); gate(26, isr26); gate(27, isr27);
    gate(28, isr28); gate(29, isr29); gate(30, isr30); gate(31, isr31);
    gate(32, isr32); gate(33, isr33); gate(34, isr34); gate(35, isr35);
    gate(36, isr36); gate(37, isr37); gate(38, isr38); gate(39, isr39);
    gate(40, isr40); gate(41, isr41); gate(42, isr42); gate(43, isr43);
    gate(44, isr44); gate(45, isr45); gate(46, isr46); gate(47, isr47);

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr = {
        .limit = sizeof(idt) - 1,
        .base  = (uint64_t)idt,
    };
    __asm__ volatile("lidt %0" :: "m"(idtr));
    pic_init();
    irqs_live = 1;
}
