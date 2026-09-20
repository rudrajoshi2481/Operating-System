.section .rodata
.global _user_hello_start, _user_hello_end
.align 12
_user_hello_start:
    .incbin "build/user/hello.elf"
_user_hello_end:
