#!/usr/bin/env python3
"""Build the repro link program from your own Emerald ROM dump.

Emerald's ROM contains the unmodified Colosseum link program. This extracts
it and applies the repro patch, producing the program that
patch_colosseum_iso.py puts on the disc (Route B).

usage:
  make_program.py ROM_DUMP colosseum-mb-repro.bps OUTPUT.gba

Standard library only. ROM_DUMP is only read.
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bps                                  # noqa: E402
from emerald_rom import PROGRAM_SIZE, find_program   # noqa: E402


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    rom = open(argv[1], 'rb').read()
    pos = find_program(rom)
    print('Colosseum link program found at ROM offset 0x%06X' % pos)
    program = bps.apply(rom[pos:pos + PROGRAM_SIZE], open(argv[2], 'rb').read())
    open(argv[3], 'wb').write(program)
    print('wrote %s (%d bytes, game code %s, SHA-1 %s)' % (
        argv[3], len(program), program[0xAC:0xB0].decode('ascii'), hashlib.sha1(program).hexdigest()))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
