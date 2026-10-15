"""Emerald (BPEE) changes for the XD link hook.

Emerald's copyright screen code (SetUpCopyrightScreen, case 141) checks the
received program's game code and swaps in its own Colosseum program:

    0816CE52  ldr r6, =0x020000AC        becomes  ldr r0, =LinkHook+1
    0816CE54  ldr r4, [r6]                        bl  _call_via_r0
    0816CE56  ldr r0, =0x65366347
    0816CE58  cmp r4, r0                          b   0x0816CE6C
    ...       (the swap, now done by LinkHook; left as dead code)
    0816CE6C  adds r0, r5, #0                     (unchanged: run the program)
    0816CE88  .word 0x089AA144                    .word LinkHook+1

linkhook.bin goes in the unused tail of the area that holds Emerald's
Colosseum program (zero in every build, also after the repro patch).
"""
import struct

HOOK_ADDR = 0x089C2000           # must match ROM_ADDR in Makefile
CODE_AT = 0x16CE52
LITERAL_AT = 0x16CE88
CALL_VIA_R0 = 0x082E70D0
RESUME = 0x0816CE6C
PROGRAM_AT = 0x9AA144
PROGRAM_SIZE = 0x28000

ORIGINAL_CODE = bytes.fromhex('0b4e' '3468' '0b48' '8442')     # ldr r6; ldr r4,[r6]; ldr r0; cmp r4,r0
ORIGINAL_LITERALS = struct.pack('<III', 0x020000AC, 0x65366347, 0x089AA144)
CALL_VIA_R0_CODE = bytes.fromhex('0047c046' '0847c046')


def thumb_bl(at, target):
    off = target - (at + 4)
    assert -0x400000 <= off < 0x400000 and not off & 1
    return struct.pack('<HH', 0xF000 | ((off >> 12) & 0x7FF), 0xF800 | ((off >> 1) & 0x7FF))


HOOK_MAGIC = 0x4B4F4F48       # "HOOK"


def hook_info(hook_bin):
    """linkhook.bin starts with LinkHook's address, a magic word and its size."""
    entry, magic, size = struct.unpack_from('<III', hook_bin, 0)
    if magic != HOOK_MAGIC or size != len(hook_bin) or not HOOK_ADDR < entry < HOOK_ADDR + size or not entry & 1:
        raise ValueError('linkhook.bin is damaged')
    return entry


def changes(rom, hook_bin):
    """[(offset, new bytes)] that install the hook in 'rom' (a dump of BPEE, with or
    without the repro Colosseum program). Refuses anything unexpected."""
    link_hook = hook_info(hook_bin)
    if bytes(rom[0xAC:0xB0]) != b'BPEE':
        raise ValueError('not US Emerald (BPEE)')
    if bytes(rom[CODE_AT:CODE_AT + 8]) != ORIGINAL_CODE:
        raise ValueError('the copyright screen code is not the one this patch expects')
    if bytes(rom[LITERAL_AT - 8:LITERAL_AT + 4]) != ORIGINAL_LITERALS:
        raise ValueError('the copyright screen literals are not the ones this patch expects')
    if bytes(rom[CALL_VIA_R0 - 0x08000000:CALL_VIA_R0 - 0x08000000 + 8]) != CALL_VIA_R0_CODE:
        raise ValueError('_call_via_r0 is not where this patch expects it')
    hook_off = HOOK_ADDR - 0x08000000
    tail_end = PROGRAM_AT + PROGRAM_SIZE
    if not (PROGRAM_AT < hook_off and hook_off + len(hook_bin) <= tail_end):
        raise ValueError('linkhook.bin does not fit in the Colosseum program area')
    if any(rom[hook_off:hook_off + len(hook_bin)]):
        raise ValueError('the area for linkhook.bin is not empty')
    code = struct.pack('<H', 0x480D)                       # ldr r0, [pc, #0x34] -> 0x0816CE88
    code += thumb_bl(0x08000000 + CODE_AT + 2, CALL_VIA_R0)
    code += struct.pack('<H', 0xE000 | (((RESUME - (0x08000000 + CODE_AT + 6 + 4)) >> 1) & 0x7FF))
    assert len(code) == 8
    return [(CODE_AT, code), (LITERAL_AT, struct.pack("<I", link_hook)), (hook_off, bytes(hook_bin))]


def apply(rom, hook_bin):
    out = bytearray(rom)
    for off, data in changes(rom, hook_bin):
        out[off:off + len(data)] = data
    return out
