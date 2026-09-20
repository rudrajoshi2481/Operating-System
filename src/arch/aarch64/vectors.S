.section .text
.p2align 11
.global exception_vectors
exception_vectors:
    .p2align 7
    b panic_entry
    .p2align 7
    b panic_entry
    .p2align 7
    b panic_entry
    .p2align 7
    b panic_entry
    .p2align 7
    b panic_entry
    .p2align 7
    b panic_entry
    .p2align 7
    b panic_entry
    .p2align 7
    b panic_entry
    .p2align 7
    b panic_entry
    .p2align 7
    b panic_entry
    .p2align 7
    b panic_entry
    .p2align 7
    b panic_entry
    .p2align 7
    b panic_entry
    .p2align 7
    b panic_entry
    .p2align 7
    b panic_entry
    .p2align 7
    b panic_entry

panic_entry:
    adrp x9, in_panic
    add  x9, x9, :lo12:in_panic
    ldr  w10, [x9]
    cbnz w10, 2f
    mov  w10, #1
    str  w10, [x9]

    adrp x9, uart_ready
    add  x9, x9, :lo12:uart_ready
    ldr  w10, [x9]
    cbz  w10, 2f
    bl   panic_dump
2:  wfi
    b    2b

.section .bss
.global in_panic
in_panic:
    .word 0
