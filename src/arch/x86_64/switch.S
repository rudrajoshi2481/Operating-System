.section .text

# swtch(uint64_t *save_sp, uint64_t load_sp)
# Push callee-saved regs (r15,r14,r13,r12,rbx,rbp), store rsp into
# *save_sp, switch to load_sp, pop the same frame, ret. A preempted
# thread's resume path unwinds back through the irq stub -> iretq.
.global swtch
swtch:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov  %rsp, (%rdi)
    mov  %rsi, %rsp
    pop  %r15
    pop  %r14
    pop  %r13
    pop  %r12
    pop  %rbx
    pop  %rbp
    ret

# First entry for a new thread. The fabricated switch frame puts fn in
# r15 and arg in r14; ret lands here with sched_lock held, IRQs masked.
.global thread_trampoline
thread_trampoline:
    call sched_thread_entry      # releases sched_lock, sti
    mov  %r14, %rdi
    call *%r15
    call thread_exit             # never returns
1:  hlt
    jmp  1b
