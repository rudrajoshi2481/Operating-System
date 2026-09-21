# isr.S — x86_64 IDT stubs.
# Exceptions 0-31 -> exc_common (dump + hcf).
# IRQ vectors 32-47 -> irq_common -> irq_dispatch(frame); a timer tick
# may swtch() away mid-handler, resuming through the pop/iretq path.

.section .text

.macro EXC vec
    .global isr\vec
isr\vec:
    push $0                      # dummy error code
    push $\vec
    jmp  exc_entry
.endm

.macro EXC_ERR vec               # vectors with hw error code
    .global isr\vec
isr\vec:
    push $\vec
    jmp  exc_entry
.endm

.macro IRQ vec
    .global isr\vec
isr\vec:
    push $0
    push $\vec
    jmp  irq_entry
.endm

EXC 0;  EXC 1;  EXC 2;  EXC 3;  EXC 4;  EXC 5;  EXC 6;  EXC 7
EXC_ERR 8;  EXC 9;  EXC_ERR 10; EXC_ERR 11; EXC_ERR 12; EXC_ERR 13
EXC_ERR 14; EXC 15; EXC 16; EXC_ERR 17; EXC 18; EXC 19; EXC 20
EXC_ERR 21; EXC 22; EXC 23; EXC 24; EXC 25; EXC 26; EXC 27; EXC 28
EXC_ERR 29; EXC_ERR 30; EXC 31

IRQ 32; IRQ 33; IRQ 34; IRQ 35; IRQ 36; IRQ 37; IRQ 38; IRQ 39
IRQ 40; IRQ 41; IRQ 42; IRQ 43; IRQ 44; IRQ 45; IRQ 46; IRQ 47

# common prologue: push all 15 GP regs (frame = {r15..r8,rbp,rdi,rsi,
# rdx,rcx,rbx,rax, vec, err, rip, cs, rflags, [rsp,ss]})
.macro SAVE_ALL
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
.endm

.macro RESTORE_ALL
    pop  %r15
    pop  %r14
    pop  %r13
    pop  %r12
    pop  %r11
    pop  %r10
    pop  %r9
    pop  %r8
    pop  %rbp
    pop  %rdi
    pop  %rsi
    pop  %rdx
    pop  %rcx
    pop  %rbx
    pop  %rax
    add  $16, %rsp               # vec + err
    iretq
.endm

exc_entry:
    SAVE_ALL
    mov  %rsp, %rdi
    call exc_handler             # dumps + hcf; never returns

irq_entry:
    SAVE_ALL
    mov  %rsp, %rdi
    call irq_dispatch
    RESTORE_ALL
