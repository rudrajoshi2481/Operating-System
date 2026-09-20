.section .bss
.p2align 4
stack_bottom:
    .space 16384
stack_top:

.section .text
.global _start
.type _start, %function
_start:
    adrp x9, stack_top
    add  x9, x9, :lo12:stack_top
    msr  spsel, #0
    mov  sp, x9
    msr  spsel, #1
    mov  sp, x9
    mov  fp, xzr

    adrp x9, exception_vectors
    add  x9, x9, :lo12:exception_vectors
    msr  vbar_el1, x9
    isb

    bl   kernel_main
1:  wfi
    b    1b
