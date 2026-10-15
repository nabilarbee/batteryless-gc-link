@ u32 HashWords(const void *src, u32 bytes, u32 h)
@ FNV-1a over little-endian 32-bit words: h = (h ^ w) * 0x01000193.
@ 'bytes' must be a multiple of 32 and 'src' word aligned. Runs from IWRAM
@ so that hashing the cart ROM is limited by the cart bus, not by fetches.

    .section .iwram, "ax", %progbits
    .arm
    .align  2
    .global HashWords
    .type   HashWords, %function
HashWords:
    push    {r4-r11}
    ldr     r12, =0x01000193
    cmp     r1, #0
    beq     2f
1:  ldmia   r0!, {r3-r10}
    eor     r2, r2, r3
    mul     r11, r2, r12
    eor     r2, r11, r4
    mul     r11, r2, r12
    eor     r2, r11, r5
    mul     r11, r2, r12
    eor     r2, r11, r6
    mul     r11, r2, r12
    eor     r2, r11, r7
    mul     r11, r2, r12
    eor     r2, r11, r8
    mul     r11, r2, r12
    eor     r2, r11, r9
    mul     r11, r2, r12
    eor     r2, r11, r10
    mul     r11, r2, r12
    mov     r2, r11
    subs    r1, r1, #32
    bne     1b
2:  mov     r0, r2
    pop     {r4-r11}
    bx      lr
    .pool
    .size   HashWords, . - HashWords
