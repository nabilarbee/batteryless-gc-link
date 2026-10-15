#!/usr/bin/env python3
"""Route A: put the repro link program into an Emerald ROM dump.

Pokemon Emerald carries its own copy of the Colosseum link program and runs
it in place of the one the GameCube sends. This finds that copy in your ROM
dump, applies the repro patch to it and writes a patched ROM. Use the result
in Dolphin's GBA to test reading the save, or flash it with a cart writer.

usage:
  patch_rom.py ROM_DUMP colosseum-mb-repro.bps OUTPUT_ROM

Standard library only. ROM_DUMP itself is never modified.
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bps                                  # noqa: E402
from emerald_rom import PROGRAM_SIZE, find_program, find_saves   # noqa: E402


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    rom_path, patch_path, out_path = argv[1:]
    if os.path.abspath(rom_path) == os.path.abspath(out_path):
        raise SystemExit('error: write the patched ROM to a new file')
    rom = bytearray(open(rom_path, 'rb').read())
    if rom[0xAC:0xB0] != b'BPEE':
        print('warning: ROM game code is %r, not BPEE (US Emerald)' % bytes(rom[0xAC:0xB0]))

    pos = find_program(rom)
    print('Colosseum link program found at ROM offset 0x%06X' % pos)
    clean = bytes(rom[pos:pos + PROGRAM_SIZE])
    repro = bps.apply(clean, open(patch_path, 'rb').read())

    saves = find_saves(rom)
    print('save area: ' + (', '.join('0x%06X' % s for s in saves) if saves else 'NONE FOUND'))

    rom[pos:pos + PROGRAM_SIZE] = repro
    mirror = pos + 0x1000000
    if len(rom) >= mirror + PROGRAM_SIZE and bytes(rom[mirror:mirror + PROGRAM_SIZE]) == clean:
        rom[mirror:mirror + PROGRAM_SIZE] = repro
        print('also patched the mirror copy at 0x%06X' % mirror)

    open(out_path, 'wb').write(rom)
    print('wrote %s (SHA-1 %s)' % (out_path, hashlib.sha1(rom).hexdigest()))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
