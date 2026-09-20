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
