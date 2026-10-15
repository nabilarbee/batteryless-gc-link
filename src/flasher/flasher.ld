/* Multiboot image: everything loads at 0x02000000 (EWRAM). ARM code in
   .iwram is copied to IWRAM by crt0; .bss lives in IWRAM. make_flasher.py
   appends the per-dump data at the first 32-byte boundary after
   __image_end. */
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY
{
    ewram (rwx) : ORIGIN = 0x02000000, LENGTH = 0x40000
    iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 0x7C00
}

SECTIONS
{
    .text :
    {
        build/crt0.o(.crt0);
        *(.text .text.*);
        *(.glue_7) *(.glue_7t);
        *(.rodata .rodata.*);
        *(.data .data.*);
        . = ALIGN(4);
    } > ewram

    .iwram :
    {
        __iwram_start = .;
        *(.iwram .iwram.*);
        . = ALIGN(4);
        __iwram_end = .;
    } > iwram AT > ewram
    __iwram_lma = LOADADDR(.iwram);
    __image_end = LOADADDR(.iwram) + SIZEOF(.iwram);

    .bss (NOLOAD) :
    {
        __bss_start = .;
        *(.bss .bss.*);
        *(COMMON);
        . = ALIGN(4);
        __bss_end = .;
    } > iwram

    /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) }
}
