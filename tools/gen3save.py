"""Pokemon Emerald save files (128 KiB: two slots of 14 sectors, then four
extra sectors): validation and a one-line summary for the tools' messages."""
import struct

SECTOR = 0x1000
SIGNATURE = 0x08012025
SAVE_SIZE = 0x20000
# Bytes covered by each sector's checksum in Emerald, by sector id
EMERALD_SIZES = [0xF2C, 0xF80, 0xF80, 0xF80, 0xF08] + [0xF80] * 8 + [0x7D0]

_CHARS = {0x00: ' ', 0xAB: '!', 0xAC: '?', 0xAD: '.', 0xAE: '-', 0xB8: ',', 0xBA: '/'}
_CHARS.update({0xA1 + i: chr(ord('0') + i) for i in range(10)})
_CHARS.update({0xBB + i: chr(ord('A') + i) for i in range(26)})
_CHARS.update({0xD5 + i: chr(ord('a') + i) for i in range(26)})


def checksum(data):
    s = sum(struct.unpack('<%dI' % (len(data) // 4), data)) & 0xFFFFFFFF
    return ((s >> 16) + s) & 0xFFFF


def check_slot(save, slot):
    """Returns (counter, offset of sector id 0, problems) for one slot."""
    ids, counters, problems = {}, set(), []
    for i in range(14):
        off = (slot * 14 + i) * SECTOR
        sid, chk, sig, counter = struct.unpack_from('<HHII', save, off + 0xFF4)
        if sig != SIGNATURE:
            problems.append('sector %d is empty' % (slot * 14 + i))
            continue
        if sid >= 14 or sid in ids:
            problems.append('sector %d has a bad id' % (slot * 14 + i))
            continue
        ids[sid] = off
        counters.add(counter)
        if checksum(save[off:off + EMERALD_SIZES[sid]]) != chk:
            problems.append('sector %d fails its checksum' % (slot * 14 + i))
    if len(counters) > 1:
        problems.append('sectors disagree on the save count')
    return (max(counters) if counters else None), ids.get(0), problems


def newest(save):
    """(slot, counter, offset of sector id 0) of the slot Emerald will load,
    i.e. the complete valid slot with the higher save count, or None."""
    best = None
    for slot in range(2):
        counter, sb2, problems = check_slot(save, slot)
        if not problems and (best is None or counter > best[1]):
            best = (slot, counter, sb2)
    return best


def summary(save):
    n = newest(save)
    if n is None:
        return 'no valid save'
    _, counter, sb2 = n
    name = ''.join(_CHARS.get(b, '?') for b in save[sb2:sb2 + 7].split(b'\xff')[0])
    tid = struct.unpack_from('<H', save, sb2 + 0x0A)[0]
    hours, minutes = struct.unpack_from('<HB', save, sb2 + 0x0E)
    return '%s, ID %05d, %d:%02d played, saved %d times' % (name, tid, hours, minutes, counter)
