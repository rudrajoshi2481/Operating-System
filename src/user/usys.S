.section .text

/* syscall ABI: nr in x8, args x0-x5, ret in x0 */
.global sys_write
sys_write:
    mov x8, #0
    svc #0
    ret

.global sys_exit
sys_exit:
    mov x8, #1
    svc #0
    ret

.global sys_yield
sys_yield:
    mov x8, #2
    svc #0
    ret

.global sys_uri
sys_uri:
    mov x8, #3
    svc #0
    ret
