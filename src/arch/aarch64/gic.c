#include "gic.h"

#define GICD_PHYS 0x08000000UL
#define GICR_PHYS 0x080a0000UL

#define GICD_CTLR      0x000
#define GICD_IGROUPR   0x080
#define GICD_ISENABLER 0x100
#define GICD_IPRIORITY 0x400

#define GICR_WAKER     0x014
#define GICR_SGI       0x10000
#define GICR_IGROUPR0  (GICR_SGI + 0x080)
#define GICR_ISENABLER0 (GICR_SGI + 0x100)
#define GICR_IPRIORITYR (GICR_SGI + 0x400)

#define GICD_CTLR_EN_GRP1NS (1u << 1)
#define GICD_CTLR_ARE_NS    (1u << 5)
#define GICD_CTLR_RWP       (1u << 31)

#define GICR_WAKER_PSLEEP (1u << 1)
#define GICR_WAKER_ASLEEP (1u << 2)

static volatile uint32_t *gicd;
static volatile uint32_t *gicr;

#define SYSREG_READ(name, out)  __asm__ volatile("mrs %0, " name : "=r"(out))
#define SYSREG_WRITE(name, in)  __asm__ volatile("msr " name ", %0" :: "r"(in))

void gic_init(uint64_t hhdm_offset)
{
    gicd = (volatile uint32_t *)(GICD_PHYS + hhdm_offset);
    gicr = (volatile uint32_t *)(GICR_PHYS + hhdm_offset);

    uint64_t sre;
    SYSREG_READ("icc_sre_el1", sre);
    SYSREG_WRITE("icc_sre_el1", sre | 1);
    __asm__ volatile("isb" ::: "memory");

    gicr[GICR_WAKER / 4] &= ~GICR_WAKER_PSLEEP;
    while (gicr[GICR_WAKER / 4] & GICR_WAKER_ASLEEP)
        ;

    uint64_t pmr = 0xff;
    SYSREG_WRITE("icc_pmr_el1", pmr);
    uint64_t igrpen = 1;
    SYSREG_WRITE("icc_igrpen1_el1", igrpen);
    __asm__ volatile("isb" ::: "memory");

    gicd[GICD_CTLR / 4] |= GICD_CTLR_EN_GRP1NS | GICD_CTLR_ARE_NS;
    while (gicd[GICD_CTLR / 4] & GICD_CTLR_RWP)
        ;
}

void gic_enable(uint32_t intid, uint8_t priority)
{
    if (intid < 32) {
        gicr[GICR_IGROUPR0 / 4] |= (1u << intid);
        ((volatile uint8_t *)gicr)[GICR_IPRIORITYR + intid] = priority;
        gicr[GICR_ISENABLER0 / 4] = (1u << intid);
    } else {
        gicd[(GICD_IGROUPR / 4) + intid / 32] |= (1u << (intid % 32));
        ((volatile uint8_t *)gicd)[GICD_IPRIORITY + intid] = priority;
        gicd[(GICD_ISENABLER / 4) + intid / 32] = (1u << (intid % 32));
    }
}

uint64_t gic_acknowledge(void)
{
    uint64_t iar;
    SYSREG_READ("icc_iar1_el1", iar);
    return iar;
}

void gic_eoi(uint64_t iar)
{
    SYSREG_WRITE("icc_eoir1_el1", iar);
    __asm__ volatile("isb" ::: "memory");
}
