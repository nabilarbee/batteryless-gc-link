/* The XD shim runs from the top of EWRAM, above everything XD's programs use
   (image, .bss and the save blocks they allocate after it; analyze_xd.py
   checks this for each program). linkhook.c copies it there. */
OUTPUT_ARCH(arm)

SECTIONS
{
    .text 0x0203E800 :
    {
        __shim_start = .;
        *(.text .text.*);
        *(.glue_7) *(.glue_7t);
        *(.rodata .rodata.*);
        *(.data .data.*);
        . = ALIGN(4);
        __shim_image_end = .;
    }

    .bss (NOLOAD) :
    {
        __shim_bss_start = .;
        *(.bss .bss.*);
        *(COMMON);
        . = ALIGN(4);
        __shim_bss_end = .;
    }

    /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) }
}
ASSERT(__shim_bss_end <= 0x0203FF80, "the shim is too large for the top of EWRAM")
