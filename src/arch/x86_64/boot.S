.section .bss
.p2align 4
stack_bottom:
    .space 16384
stack_top:

.section .text
.global _start
_start:
    cli
    mov  $stack_top, %rsp
    xor  %rbp, %rbp
    call kernel_main
1:  hlt
    jmp  1b
