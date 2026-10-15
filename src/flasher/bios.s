@ u32 BiosMidiKey2Freq(u32 wave, u32 key, u32 fine): BIOS call 0x1F, used by
@ the BIOS dump (it reads BIOS memory while the BIOS code runs).

    .text
    .thumb
    .align  2
    .global BiosMidiKey2Freq
    .thumb_func
BiosMidiKey2Freq:
    swi     0x1F
    bx      lr
