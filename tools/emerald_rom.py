"""Helpers shared by the repro tools: locate things inside an Emerald ROM dump."""
import hashlib
import struct

# pret/colosseum-mb, Emerald build: the copy Emerald carries in its ROM
CLEAN_PROGRAM_SHA1 = '6cf444e998c39b74046c9715f3a07033b3262d12'
PROGRAM_SIZE = 0x28000
SIGNATURE = 0x08012025


def find_program(rom):
    """Offset of Emerald's unmodified Colosseum link program, or raise."""
    search = rom[:0x1000000]          # the game is in the first 16 MiB
    hits = []
    pos = search.find(b'\x00\x00\x00\x00TEST01', 0)
    while pos >= 0:
        start = pos - 0xA8            # game code "TEST" sits at +0xAC, title before it
        if start >= 0 and hashlib.sha1(search[start:start + PROGRAM_SIZE]).hexdigest() == CLEAN_PROGRAM_SHA1:
            hits.append(start)
        pos = search.find(b'\x00\x00\x00\x00TEST01', pos + 1)
    if len(hits) != 1:
        raise SystemExit('error: expected one unmodified Colosseum link program in the ROM, found %d '
                         '(is this a US Emerald dump, and is it already patched?)' % len(hits))
    return hits[0]


def find_saves(rom):
    """64 KiB-aligned save areas with at least one complete slot."""
    found = []
    for base in range(0, len(rom) - 0x20000 + 1, 0x10000):
        complete = 0
        for slot in range(2):
            ids, counters = set(), set()
            for i in range(14):
                off = base + (slot * 14 + i) * 0x1000
                sid, _, sig, counter = struct.unpack_from('<HHII', rom, off + 0xFF4)
                if sig == SIGNATURE:
                    ids.add(sid)
                    counters.add(counter)
            if ids == set(range(14)) and len(counters) == 1:
                complete += 1
        if complete:
            found.append(base)
    return found
