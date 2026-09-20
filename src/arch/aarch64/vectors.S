.section .text

.macro SAVE_FRAME
    sub  sp, sp, #272
    stp  x0,  x1,  [sp, #0]
    stp  x2,  x3,  [sp, #16]
    stp  x4,  x5,  [sp, #32]
    stp  x6,  x7,  [sp, #48]
    stp  x8,  x9,  [sp, #64]
    stp  x10, x11, [sp, #80]
    stp  x12, x13, [sp, #96]
    stp  x14, x15, [sp, #112]
    stp  x16, x17, [sp, #128]
    stp  x18, x19, [sp, #144]
    stp  x20, x21, [sp, #160]
    stp  x22, x23, [sp, #176]
    stp  x24, x25, [sp, #192]
    stp  x26, x27, [sp, #208]
    stp  x28, x29, [sp, #224]
    str  x30,      [sp, #240]
    mrs  x9,  elr_el1
    mrs  x10, spsr_el1
    stp  x9, x10, [sp, #248]
.endm

.macro RESTORE_FRAME
    ldp  x9, x10, [sp, #248]
    msr  elr_el1, x9
    msr  spsr_el1, x10
    ldr  x30, [sp, #240]
    ldp  x28, x29, [sp, #224]
    ldp  x26, x27, [sp, #208]
    ldp  x24, x25, [sp, #192]
    ldp  x22, x23, [sp, #176]
    ldp  x20, x21, [sp, #160]
    ldp  x18, x19, [sp, #144]
    ldp  x16, x17, [sp, #128]
    ldp  x14, x15, [sp, #112]
    ldp  x12, x13, [sp, #96]
    ldp  x10, x11, [sp, #80]
    ldp  x8,  x9,  [sp, #64]
    ldp  x6,  x7,  [sp, #48]
    ldp  x4,  x5,  [sp, #32]
    ldp  x2,  x3,  [sp, #16]
    ldp  x0,  x1,  [sp, #0]
    add  sp, sp, #272
.endm

.p2align 11
.global exception_vectors
exception_vectors:
    b sync_entry            // 0x000 sync,   same EL using SP0
    .p2align 7
    b irq_entry             // 0x080 IRQ,    same EL using SP0
    .p2align 7
    b fiq_entry             // 0x100 FIQ,    same EL using SP0
    .p2align 7
    b serr_entry            // 0x180 SError, same EL using SP0
    .p2align 7
    b sync_entry            // 0x200 sync,   same EL using SPx
    .p2align 7
    b irq_entry             // 0x280 IRQ,    same EL using SPx
    .p2align 7
    b fiq_entry             // 0x300 FIQ,    same EL using SPx
    .p2align 7
    b serr_entry            // 0x380 SError, same EL using SPx
    .p2align 7
    b sync_lower_entry      // 0x400 sync,   lower EL aarch64 (svc)
    .p2align 7
    b irq_entry             // 0x480 IRQ,    lower EL aarch64
    .p2align 7
    b fiq_entry             // 0x500 FIQ,    lower EL aarch64
    .p2align 7
    b serr_entry            // 0x580 SError, lower EL aarch64
    .p2align 7
    b lower_entry           // 0x600 sync,   lower EL aarch32
    .p2align 7
    b irq_entry             // 0x680 IRQ,    lower EL aarch32
    .p2align 7
    b fiq_entry             // 0x700 FIQ,    lower EL aarch32
    .p2align 7
    b serr_entry            // 0x780 SError, lower EL aarch32
    .p2align 7

sync_entry:
    SAVE_FRAME
    mov  x0, sp
    bl   sync_exception
    b    halt_loop

irq_entry:
    SAVE_FRAME
    mov  x0, sp
    bl   irq_dispatch
.global ret_to_el0
ret_to_el0:
    RESTORE_FRAME
    eret

sync_lower_entry:
    SAVE_FRAME
    mov  x0, sp
    bl   sync_lower
    b    ret_to_el0

fiq_entry:
    SAVE_FRAME
    mov  x0, sp
    mov  w1, #1
    bl   fatal_exception
    b    halt_loop

serr_entry:
    SAVE_FRAME
    mov  x0, sp
    mov  w1, #2
    bl   fatal_exception
    b    halt_loop

lower_entry:
    SAVE_FRAME
    mov  x0, sp
    mov  w1, #3
    bl   fatal_exception
    b    halt_loop

halt_loop:
    wfi
    b    halt_loop
