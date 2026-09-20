.section .text

// swtch(uint64_t *save_sp, uint64_t load_sp)
// Saves callee-saved regs + lr on the current stack, stores sp into
// *save_sp, switches to load_sp, pops the same frame, returns.
// A preempted thread's resume path unwinds back through irq_dispatch ->
// RESTORE_FRAME -> eret; a yielded thread returns into its caller.
.global swtch
swtch:
    sub  sp, sp, #96
    stp  x19, x20, [sp, #0]
    stp  x21, x22, [sp, #16]
    stp  x23, x24, [sp, #32]
    stp  x25, x26, [sp, #48]
    stp  x27, x28, [sp, #64]
    stp  x29, x30, [sp, #80]
    mov  x9, sp
    str  x9, [x0]
    mov  sp, x1
    ldp  x19, x20, [sp, #0]
    ldp  x21, x22, [sp, #16]
    ldp  x23, x24, [sp, #32]
    ldp  x25, x26, [sp, #48]
    ldp  x27, x28, [sp, #64]
    ldp  x29, x30, [sp, #80]
    add  sp, sp, #96
    ret

// First entry for a new thread. The fabricated switch frame puts fn in
// x19 and arg in x20; lr = this trampoline. We arrive here with
// sched_lock held and IRQs masked by whoever switched to us.
.global thread_trampoline
thread_trampoline:
    bl   sched_thread_entry     // releases sched_lock, unmasks IRQs
    mov  x0, x20
    blr  x19
    bl   thread_exit            // never returns
1:  wfe
    b    1b

// First entry for a user thread. Arrives with sched_lock held and IRQs
// masked; user_thread_enter() drops the lock and calls enter_el0().
.global user_trampoline
user_trampoline:
    bl   user_thread_enter      // never returns

// enter_el0(uentry=x0, usp=x1, pgd_phys=x2, kstack_top=x3)
// Installs the process address space, builds a zeroed trap frame at the
// top of the kernel stack, and erets into EL0.
.global enter_el0
enter_el0:
    msr  ttbr0_el1, x2
    msr  sp_el0, x1
    dsb  ishst
    tlbi vmalle1
    dsb  sy
    isb
    mov  sp, x3
    sub  sp, sp, #272
    mov  x9, sp
    mov  x10, #17                 /* 17 * 16 = 272 bytes */
1:  stp  xzr, xzr, [x9], #16
    subs x10, x10, #1
    b.ne 1b
    str  x0, [sp, #248]           /* elr = user entry point */
    str  xzr, [sp, #256]          /* spsr = EL0t, DAIF clear (IRQs on) */
    b    ret_to_el0
