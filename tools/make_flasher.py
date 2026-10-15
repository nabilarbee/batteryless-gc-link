#!/usr/bin/env python3
"""Builds the Emerald link flasher for your cart.

The flasher is a GBA multiboot program. Sent to the GBA over the GameCube
link cable with your repro Emerald cart inserted, it writes two things into
the cart's ROM so that unmodified discs work with it:
  - the repro build of the Colosseum link program, over Emerald's own copy
    (the same as the v0.2 flasher; already done if you ran that one);
  - the XD link hook: a few bytes of Emerald's link code, and the hook and
    XD save code (linkhook.bin) in unused space after the Colosseum program.

usage:
  make_flasher.py ROM_DUMP colosseum-mb-repro.bps OUTPUT.gba

ROM_DUMP must be a dump of the very cart you will flash, taken before any
flash (or of an identical cart). The flasher checks the cart against it and
refuses to write to anything else. The dump needs to be current only outside
the save area, so an older dump of the same cart is fine. Standard library
only. ROM_DUMP itself is never modified.
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'xdlink'))
import bps                                              # noqa: E402
import emerald_hook                                     # noqa: E402
from emerald_rom import PROGRAM_SIZE, find_program, find_saves   # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
CODE_PATH = os.path.join(HERE, 'flasher_code.bin')
HOOK_PATH = os.path.join(HERE, 'linkhook.bin')
# Hooks written by earlier flashers, so that a cart flashed with them is
# recognised and brought up to date.
PREVIOUS_HOOKS = [os.path.join(HERE, 'linkhook_v0.5.bin')]

MAGIC = 0x48534C46          # "FLSH"
VERSION = 2
HASH_CHUNK = 0x1000
HASH_BASIS = 0x811C9DC5
MAX_ID_RANGES = 8
MAX_SPANS = 4
MAX_SOURCES = 3
CONFIG_SIZE = 0x140
BIG_BLOCK = 0x40000         # largest erase block the flasher accepts
SAVE_SIZE = 0x20000
GAME_AREA = 0x1000000       # Emerald uses the first 16 MiB of the cart
MULTIBOOT_LIMIT = 0x40000


def fnv_words(data, h=HASH_BASIS):
    """FNV-1a over little-endian 32-bit words, as hash.s computes it."""
    for (w,) in struct.iter_unpack('<I', data):
        h = ((h ^ w) * 0x01000193) & 0xFFFFFFFF
    return h


def subtract(ranges, cut):
    out = []
    for a, b in ranges:
        if cut[1] <= a or cut[0] >= b:
            out.append((a, b))
            continue
        if a < cut[0]:
            out.append((a, cut[0]))
        if cut[1] < b:
            out.append((cut[1], b))
    return out


def plan_areas(rom, save, hash_lo, hash_hi, more=()):
    """Picks the unused areas the flasher (and the save writer) may use, and
    the ranges the identity check covers. [hash_lo, hash_hi) is the 256 KiB
    aligned area around the link program, which the flasher rewrites; 'more'
    lists further such areas."""
    written = [(hash_lo, hash_hi)] + list(more)
    save_lo = save & ~(BIG_BLOCK - 1)
    save_hi = (save + SAVE_SIZE + BIG_BLOCK - 1) & ~(BIG_BLOCK - 1)
    # Spare area: chunks that are all 0xFF in the dump.
    ff = b'\xff' * HASH_CHUNK
    free = [rom[i:i + HASH_CHUNK] == ff for i in range(0, GAME_AREA, HASH_CHUNK)]

    def all_free(a, b):
        if a < 0 or b > GAME_AREA:
            return False
        if b > save_lo and a < save_hi or any(b > lo and a < hi for lo, hi in written):
            return False
        return all(free[a // HASH_CHUNK:b // HASH_CHUNK])

    # Test block: 512 KiB aligned with 512 KiB free on each side, so that even
    # a chip whose blocks are twice what it reports cannot reach used data
    # while its block size is being checked.
    tests = [t for t in range(2 * BIG_BLOCK, GAME_AREA, 2 * BIG_BLOCK)
             if all_free(t - 2 * BIG_BLOCK, t + 2 * BIG_BLOCK)]
    if not tests:
        raise SystemExit('error: no 1 MiB unused area in the dump for the erase test')
    test = tests[-1]
    stagings = [s for s in range(0, GAME_AREA, BIG_BLOCK)
                if all_free(s, s + BIG_BLOCK) and (s + BIG_BLOCK <= test - 2 * BIG_BLOCK or s >= test + 2 * BIG_BLOCK)]
    if not stagings:
        raise SystemExit('error: no 256 KiB unused area in the dump for the spare copy')
    staging = stagings[-1]

    # The identity check covers everything that should never change.
    ranges = [(0, save_lo)]
    for cut in written + [(test - 2 * BIG_BLOCK, test + 2 * BIG_BLOCK), (staging, staging + BIG_BLOCK)]:
        ranges = subtract(ranges, cut)
    if len(ranges) > MAX_ID_RANGES:
        raise SystemExit('error: too many identity ranges')
    return test, staging, ranges


def spans_of(rom, target):
    """Groups the bytes where target differs from rom into spans (bytes less
    than 256 KiB apart go together), each with its 256 KiB aligned area."""
    runs = []
    for c in range(0, len(rom), HASH_CHUNK):
        if rom[c:c + HASH_CHUNK] == target[c:c + HASH_CHUNK]:
            continue
        for j in range(c, c + HASH_CHUNK):
            if rom[j] != target[j]:
                if runs and j - runs[-1][1] < BIG_BLOCK:
                    runs[-1][1] = j + 1
                else:
                    runs.append([j, j + 1])
    spans = [(d0, d1, d0 & ~(BIG_BLOCK - 1), (d1 + BIG_BLOCK - 1) & ~(BIG_BLOCK - 1)) for d0, d1 in runs]
    for (_, _, _, hi), (_, _, lo, _) in zip(spans, spans[1:]):
        if lo < hi:
            raise SystemExit('error: areas to write too close together')
    return spans


def chunk_hashes(data, lo, hi):
    return [fnv_words(data[a:a + HASH_CHUNK]) for a in range(lo, hi, HASH_CHUNK)]


def build(rom, patch, code, hook, previous_hooks=()):
    if len(rom) < GAME_AREA:
        raise SystemExit('error: the dump is smaller than 16 MiB')
    if rom[0xAC:0xB0] != b'BPEE':
        raise SystemExit('error: game code is %r, not BPEE (US Emerald)' % bytes(rom[0xAC:0xB0]))

    # What the cart looks like after the v0.2 flasher (Colosseum program
    # only), and after this one.
    pos = find_program(rom)
    new = bps.apply(bytes(rom[pos:pos + PROGRAM_SIZE]), patch)
    if len(new) != PROGRAM_SIZE:
        raise SystemExit('error: the patch gives a program of the wrong size')
    colosseum = bytearray(rom)
    colosseum[pos:pos + PROGRAM_SIZE] = new
    try:
        target = emerald_hook.apply(colosseum, hook)
        earlier = [rom, colosseum] + [emerald_hook.apply(colosseum, h) for h in previous_hooks]
    except ValueError as e:
        raise SystemExit('error: %s' % e)

    saves = find_saves(rom)
    if len(saves) != 1:
        raise SystemExit('error: expected one save area in the dump, found %d' % len(saves))
    save = saves[0]
    save_lo = save & ~(BIG_BLOCK - 1)
    save_hi = (save + SAVE_SIZE + BIG_BLOCK - 1) & ~(BIG_BLOCK - 1)

    spans = spans_of(rom[:GAME_AREA], target[:GAME_AREA])
    if not spans or len(spans) > MAX_SPANS:
        raise SystemExit('error: %d areas to write' % len(spans))
    for d0, d1, lo, hi in spans:
        # Whatever the batteryless code may erase around the save is off limits.
        if hi > save_lo and lo < save_hi:
            raise SystemExit('error: an area to write is too close to the save area')
    test, staging, ranges = plan_areas(rom, save, spans[0][2], spans[0][3], [(lo, hi) for _, _, lo, hi in spans[1:]])
    id_hash = HASH_BASIS
    for a, b in ranges:
        id_hash = fnv_words(rom[a:b], id_hash)

    span_hdr = b''
    tail = b''
    base = CONFIG_SIZE
    for d0, d1, lo, hi in spans:
        # Every state the area may be in: the dump, and the dump after each
        # earlier flasher. Already written states need no entry.
        sources = []
        for state in earlier:
            if state[lo:hi] != target[lo:hi] and all(state[lo:hi] != s[lo:hi] for s in sources):
                sources.append(state)
        if not sources or len(sources) > MAX_SOURCES:
            raise SystemExit('error: %d earlier states for 0x%X-0x%X' % (len(sources), lo, hi))
        tables = chunk_hashes(target, lo, hi)
        for src in sources:
            tables += chunk_hashes(src, lo, hi)
        tables_off = base + len(tail)
        tail += struct.pack('<%dI' % len(tables), *tables)
        bytes_off = base + len(tail)
        tail += bytes(target[d0:d1])
        tail += b'\0' * (-len(tail) % 4)
        name = b'Link programs' if d0 <= pos + PROGRAM_SIZE and pos <= d1 else b'Link code'
        span_hdr += name.ljust(16, b'\0') + struct.pack('<8I', d0, d1, lo, hi, bytes_off, tables_off, len(sources), 0)
    span_hdr += bytes(0x30 * (MAX_SPANS - len(spans)))

    body = struct.pack('<8I', struct.unpack('<I', b'BPEE')[0], save, test, staging, len(ranges), id_hash, len(spans), 0)
    if len(ranges) > MAX_ID_RANGES:
        raise SystemExit('error: too many identity ranges')
    for i in range(MAX_ID_RANGES):
        body += struct.pack('<II', *(ranges[i] if i < len(ranges) else (0, 0)))
    body += span_hdr
    assert 0x20 + len(body) == CONFIG_SIZE
    body += tail
    body += b'\0' * (-(0x20 + len(body)) % 32)
    total = 0x20 + len(body)
    config = struct.pack('<8I', MAGIC, VERSION, total, fnv_words(body), 0, 0, 0, 0) + body

    image = bytearray(code)
    image[0x04:0xA0] = rom[0x04:0xA0]          # Nintendo logo, from the dump
    chk = 0
    for b in image[0xA0:0xBD]:
        chk = (chk - b) & 0xFF
    image[0xBD] = (chk - 0x19) & 0xFF
    image += b'\0' * (-len(image) % 32)
    image += config
    if len(image) > MULTIBOOT_LIMIT:
        raise SystemExit('error: flasher is %d bytes, over the 256 KiB multiboot limit' % len(image))

    info = {
        'program': pos, 'spans': spans, 'save': save, 'test': test, 'staging': staging,
        'ranges': ranges, 'size': len(image), 'target': bytes(target), 'colosseum': bytes(colosseum),
        'earlier': [bytes(e) for e in earlier[2:]],
    }
    return bytes(image), info


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    rom_path, patch_path, out_path = argv[1:]
    if os.path.abspath(rom_path) == os.path.abspath(out_path):
        raise SystemExit('error: write the flasher to a new file')
    rom = open(rom_path, 'rb').read()
    previous = [open(p, 'rb').read() for p in PREVIOUS_HOOKS if os.path.exists(p)]
    image, info = build(rom, open(patch_path, 'rb').read(), open(CODE_PATH, 'rb').read(), open(HOOK_PATH, 'rb').read(), previous)
    open(out_path, 'wb').write(image)
    print('Colosseum link program at 0x%06X' % info['program'])
    for d0, d1, lo, hi in info['spans']:
        print('writes 0x%06X-0x%06X' % (d0, d1))
    print('save area at 0x%06X (never written)' % info['save'])
    print('spare area: copy at 0x%06X, erase test around 0x%06X' % (info['staging'], info['test']))
    print('wrote %s (%d bytes, SHA-1 %s)' % (out_path, len(image), hashlib.sha1(image).hexdigest()))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
