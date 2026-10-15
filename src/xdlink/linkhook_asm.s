@ Small pieces of the link hook that C can't express.

@ linkhook.bin starts with this: what the flasher builder needs to install it.
    .section .hookhead, "a"
    .align 2
    .word   HookEntry               @ Thumb entry (bit 0 set by the linker)
    .word   0x4B4F4F48              @ "HOOK"
    .word   __hook_size

@ Emerald's link code calls this (through _call_via_r0). It stays at offset
@ 0xD0, where the released v0.5 had LinkHook (0x089C20D1 is in Emerald's link
@ code on carts flashed with it), so those carts keep their link code block
@ and only the hook's block is rewritten.
    .section .hookentry, "ax"
    .thumb
    .align 2
    .global HookEntry
    .thumb_func
HookEntry:
    ldr     r3, 1f
    bx      r3
    .align 2
1:  .word   LinkHook

    .text
    .align 2

@ Entered in ARM state from XD's loader (ldr pc, [pc, #8]).
    .arm
    .global XdPostLoadArm
XdPostLoadArm:
    ldr     r0, =XdPostLoad
    bx      r0

    .thumb
    .align 2
    .global CpuSet
    .thumb_func
CpuSet:
    swi     0x0B
    bx      lr

@ Starts the decompressed program at 0x02000000 in ARM state, as XD's loader does.
    .global JumpToProgram
    .thumb_func
JumpToProgram:
    ldr     r0, =0x02000000
    bx      r0

    .align 2
    .pool

@ The shim, copied to EWRAM by XdPostLoad.
    .section .rodata
    .align 2
    .global gShimImage, gShimImageEnd
gShimImage:
    .incbin "build/shim.bin"
gShimImageEnd:
    .align 2
