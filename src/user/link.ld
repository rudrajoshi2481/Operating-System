OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = 0x400000;
    .text : { *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.* .sdata .sdata.*) }
    .bss : { *(.sbss .sbss.* COMMON .bss .bss.*) }
    /DISCARD/ : { *(.comment .note .note.* .eh_frame*) }
}
