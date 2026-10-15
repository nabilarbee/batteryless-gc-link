#!/usr/bin/env python3
"""Builds the Emerald save writer for your cart.

The save writer is a GBA multiboot program, sent with GBA Link Cable ROM
Sender like the flasher. It writes a 128 KiB Emerald save file into your
batteryless repro cart's save area, replacing the save that is there.

usage:
  make_save_writer.py ROM_DUMP SAVE_FILE OUTPUT.gba

ROM_DUMP is a dump of the cart, made before or after flashing; the save in
it doesn't matter. SAVE_FILE is the save to write, for
example one that extract_save.py pulled out of a dump and PKHeX edited.
Standard library only; neither input is modified.
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'xdlink'))
import gen3save                                         # noqa: E402
from emerald_hook import CODE_AT as HOOK_CODE_AT        # noqa: E402
from emerald_rom import PROGRAM_SIZE, find_saves        # noqa: E402
from make_flasher import (BIG_BLOCK, GAME_AREA, HASH_BASIS,  # noqa: E402
                          MULTIBOOT_LIMIT, SAVE_SIZE, fnv_words, plan_areas)

HERE = os.path.dirname(os.path.abspath(__file__))
CODE_PATH = os.path.join(HERE, 'savewriter_code.bin')

MAGIC = 0x57564153          # "SAVW"
VERSION = 1
CONFIG_SIZE = 0x80
MAX_ID_RANGES = 6


def program_area(rom):
    """The 256 KiB aligned area around Emerald's Colosseum link program,
    original or flashed, which the flasher may have rewritten."""
    pattern = b'\x00\x00\x00\x00TEST01'
    hits = []
    pos = rom.find(pattern, 0, GAME_AREA)
    while pos >= 0:
        hits.append(pos - 0xA8)
        pos = rom.find(pattern, pos + 1, GAME_AREA)
    if len(hits) != 1:
        raise SystemExit('error: expected one Colosseum link program in the dump, found %d' % len(hits))
    return hits[0] & ~(BIG_BLOCK - 1), (hits[0] + PROGRAM_SIZE + BIG_BLOCK - 1) & ~(BIG_BLOCK - 1)


def build(rom, save, code):
    if len(rom) < GAME_AREA:
        raise SystemExit('error: the dump is smaller than 16 MiB')
    if rom[0xAC:0xB0] != b'BPEE':
        raise SystemExit('error: game code is %r, not BPEE (US Emerald)' % bytes(rom[0xAC:0xB0]))
    if len(save) != gen3save.SAVE_SIZE:
        raise SystemExit('error: the save file is %d bytes; an Emerald save is %d' % (len(save), gen3save.SAVE_SIZE))
    if gen3save.newest(save) is None:
        problems = [p for slot in range(2) for p in gen3save.check_slot(save, slot)[2]]
        raise SystemExit('error: no valid save in the file (%s)' % '; '.join(problems[:4]))

    saves = find_saves(rom[:GAME_AREA])
    if len(saves) != 1:
        raise SystemExit('error: expected one save area in the dump, found %d' % len(saves))
    save_addr = saves[0]
    area_lo = save_addr & ~(BIG_BLOCK - 1)
    area_hi = (save_addr + SAVE_SIZE + BIG_BLOCK - 1) & ~(BIG_BLOCK - 1)
    if rom[area_lo:save_addr].strip(b'\xff') or rom[save_addr + SAVE_SIZE:area_hi].strip(b'\xff'):
        raise SystemExit('error: the dump has data next to the save area, where an erase could reach')

    # The flasher may have rewritten the link program's area and the link code
    # with the XD hook, so the cart is compared with the dump everywhere else.
    prog_lo, prog_hi = program_area(rom)
    hook_lo = HOOK_CODE_AT & ~(BIG_BLOCK - 1)
    test, _, ranges = plan_areas(rom, save_addr, prog_lo, prog_hi, [(hook_lo, hook_lo + BIG_BLOCK)])
    if len(ranges) > MAX_ID_RANGES:
        raise SystemExit('error: too many identity ranges')
    id_hash = HASH_BASIS
    for a, b in ranges:
        id_hash = fnv_words(rom[a:b], id_hash)

    body = struct.pack('<8I', struct.unpack('<I', b'BPEE')[0], save_addr, SAVE_SIZE, area_lo, area_hi,
                       test, len(ranges), id_hash)
    for i in range(MAX_ID_RANGES):
        body += struct.pack('<II', *(ranges[i] if i < len(ranges) else (0, 0)))
    body += b'\0' * 16
    assert 0x20 + len(body) == CONFIG_SIZE
    body += save
    body += b'\0' * (-(0x20 + len(body)) % 32)
    config = struct.pack('<8I', MAGIC, VERSION, 0x20 + len(body), fnv_words(body), 0, 0, 0, 0) + body

    image = bytearray(code)
    image[0x04:0xA0] = rom[0x04:0xA0]          # Nintendo logo, from the dump
    image[0xA0:0xB0] = b'EMERALDSAVEWSAVW'     # title and game code
    chk = 0
    for b in image[0xA0:0xBD]:
        chk = (chk - b) & 0xFF
    image[0xBD] = (chk - 0x19) & 0xFF
    image += b'\0' * (-len(image) % 32)
    image += config
    if len(image) > MULTIBOOT_LIMIT:
        raise SystemExit('error: the save writer is over the 256 KiB multiboot limit')
    return bytes(image), {'save': save_addr, 'test': test, 'area': (area_lo, area_hi)}


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    rom_path, save_path, out_path = argv[1:]
    if os.path.abspath(out_path) in (os.path.abspath(rom_path), os.path.abspath(save_path)):
        raise SystemExit('error: write the save writer to a new file')
    rom = open(rom_path, 'rb').read()
    save = open(save_path, 'rb').read()
    image, info = build(rom, save, open(CODE_PATH, 'rb').read())
    open(out_path, 'wb').write(image)
    print('save area at 0x%06X' % info['save'])
    print('  in the dump: %s' % gen3save.summary(rom[info['save']:info['save'] + SAVE_SIZE]))
    print('  to write:    %s' % gen3save.summary(save))
    print('wrote %s (%d bytes, SHA-1 %s)' % (out_path, len(image), hashlib.sha1(image).hexdigest()))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
