/* The link hook lives in cart ROM, in the unused tail of the area that holds
   Emerald's Colosseum program. The address is passed with --defsym ROM_ADDR. */
OUTPUT_ARCH(arm)

SECTIONS
{
    .text ROM_ADDR :
    {
        __hook_start = .;
        KEEP(build/linkhook_asm.o(.hookhead));
        . = __hook_start + 0xD0;
        KEEP(build/linkhook_asm.o(.hookentry));
        build/linkhook.o(.text);
        *(.text .text.*);
        *(.glue_7) *(.glue_7t);
        *(.rodata .rodata.*);
        *(.data .data.*);
        . = ALIGN(4);
        __hook_end = .;
    }
    __hook_size = __hook_end - __hook_start;

    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) }
}
