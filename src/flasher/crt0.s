@ Multiboot start-up for the flasher. The header fields left empty here
@ (Nintendo logo, complement check) are filled in by make_flasher.py from
@ the user's own ROM dump.

    .section .crt0, "ax", %progbits
    .arm
    .global _start
_start:
    b       start_vector            @ 0x00 cartridge entry (unused)
    .fill   156, 1, 0               @ 0x04 Nintendo logo
    .ascii  "EMERALDFLASH"          @ 0xA0 title
    .ascii  "FLSH"                  @ 0xAC game code
    .ascii  "01"                    @ 0xB0 maker code
    .byte   0x96                    @ 0xB2 fixed value
    .byte   0x00                    @ 0xB3 main unit
    .byte   0x00                    @ 0xB4 device type
    .fill   7, 1, 0                 @ 0xB5 reserved
    .byte   0x00                    @ 0xBC version
    .byte   0x00                    @ 0xBD complement check
    .fill   2, 1, 0                 @ 0xBE reserved

    b       start_vector            @ 0xC0 multiboot entry (normal/multiplay)
    .byte   0                       @ 0xC4 boot mode, written by the BIOS
    .byte   0                       @ 0xC5 slave number, written by the BIOS
    .fill   26, 1, 0                @ 0xC6 unused
    b       start_vector            @ 0xE0 multiboot entry (JOY bus)

start_vector:
    mov     r0, #0x04000000
    str     r0, [r0, #0x208]        @ IME = 0: the flasher runs without interrupts

    mov     r0, #0xD2               @ IRQ mode, interrupts masked
    msr     cpsr_c, r0
    ldr     sp, =0x03007FA0
    mov     r0, #0xDF               @ system mode, interrupts masked
    msr     cpsr_c, r0
    ldr     sp, =0x03007F00

    ldr     r0, =__iwram_lma        @ ARM code that runs from IWRAM
    ldr     r1, =__iwram_start
    ldr     r2, =__iwram_end
1:  cmp     r1, r2
    ldrlo   r3, [r0], #4
    strlo   r3, [r1], #4
    blo     1b

    ldr     r1, =__bss_start
    ldr     r2, =__bss_end
    mov     r3, #0
2:  cmp     r1, r2
    strlo   r3, [r1], #4
    blo     2b

    ldr     r0, =AgbMain
    mov     lr, pc
    bx      r0
3:  b       3b

    .pool
