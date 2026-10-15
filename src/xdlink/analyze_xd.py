#!/usr/bin/env python3
"""Finds, in one of Pokemon XD's GBA link programs, everything the batteryless
save fix needs: where the program decompresses, the flash library functions,
the call sites to redirect and the flash library's variables.

usage: analyze_xd.py PROGRAM.gba [...]   (programs from patch_colosseum_iso.py extract)

The flash library in XD's programs is byte-identical to the one in Colosseum's
program, so its functions are found by matching the compiled colosseum-mb
payload (built in ../../payload) with the literal pools and BL offsets masked.
"""
import json
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PAYLOAD = os.path.normpath(os.path.join(HERE, '..', '..', 'payload'))
BASE = 0x02000000
HASH_BASIS = 0x811C9DC5
JOY_TRANS = 0x04000154

# The IWRAM loader at the end of each program, as copied to 0x03000000: it moves
# the compressed program to the top of EWRAM, decompresses it to 0x02000000 and
# jumps there. Offsets of the parts the fix uses or changes:
LOADER_SIZE = 0x1C4
LOADER_JUMP = 0x1AC        # "bx r5" (r5 = 0x02000000)
LOADER_SPARE = 0x1BC       # epilogue that is never reached; holds the new jump target
LOADER_LITS = 0x1B4        # blob end, blob start
FLASH_LIB_SIZE = 0xC00     # SwitchFlashBank to the end of agb_flash_le
HEADER_SIZE = 0xC0         # not hashed after decompression: XD changes the game code
SHIM_ADDR = 0x0203E800     # where the hook puts the shim (shim.ld)
# The programs allocate the save blocks after their .bss: an initial gap and a
# gap after each block of up to 60 bytes, two copies of SaveBlock1, SaveBlock2
# and the PC storage. Sizes are the cart's; Emerald's are the largest.
SAVE_BLOCKS_MAX = 5 * 60 + 2 * 0x3D88 + 0xF2C + 0x83D0


def fnv_words(data, h=HASH_BASIS):
    for (w,) in struct.iter_unpack('<I', data[:len(data) // 4 * 4]):
        h = ((h ^ w) * 0x01000193) & 0xFFFFFFFF
    return h


def lz77_decode(src, pos):
    hdr = struct.unpack_from('<I', src, pos)[0]
    if hdr & 0xFF != 0x10:
        raise ValueError('not LZ77')
    size = hdr >> 8
    pos += 4
    out = bytearray()
    while len(out) < size:
        flags = src[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                v = (src[pos] << 8) | src[pos + 1]
                pos += 2
                disp = (v & 0xFFF) + 1
                for _ in range((v >> 12) + 3):
                    out.append(out[-disp])
            else:
                out.append(src[pos])
                pos += 1
    return bytes(out), pos


def loader_template(prog, off):
    """The loader with its two literals blanked, for comparing programs."""
    t = bytearray(prog[off:off + LOADER_SIZE])
    t[LOADER_LITS:LOADER_LITS + 8] = bytes(8)
    return bytes(t)


def find_loader(prog):
    """The stub's main DMA-copies the loader from the end of the image to
    0x03000000 and calls 0x03000130; the literals give the blob."""
    for off in range(len(prog) - 0x400, len(prog) - LOADER_SIZE + 1, 4):
        if off < 0:
            continue
        if prog[off + LOADER_JUMP:off + LOADER_JUMP + 4] != bytes.fromhex('15ff2fe1'):   # bx r5
            continue
        if prog[off + LOADER_JUMP - 4:off + LOADER_JUMP] != bytes.fromhex('0fe0a0e1'):   # mov lr, pc
            continue
        end, start = struct.unpack_from('<II', prog, off + LOADER_LITS)
        if BASE <= start < end <= BASE + len(prog):
            return off, start - BASE, end - BASE
    return None


# ------------------------------------------------------------------ signatures

def payload_symbols():
    elf = os.path.join(PAYLOAD, 'payload.elf')
    out = subprocess.run(['arm-none-eabi-nm', '-n', elf], capture_output=True, text=True, check=True).stdout
    funcs, alls, names = [], set(), {}
    for line in out.splitlines():
        p = line.split()
        if len(p) != 3:
            continue
        a = int(p[0], 16)
        if not BASE <= a < BASE + 0x40000:
            continue
        alls.add(a & ~1)
        names.setdefault(a & ~1, p[2])
        if p[1] in 'tTwW':
            funcs.append((a & ~1, p[2]))
    return funcs, sorted(alls), names


def thumb_mask(code, base):
    wild = bytearray(len(code))
    i = 0
    while i + 1 < len(code):
        h = struct.unpack_from('<H', code, i)[0]
        if (h & 0xF800) == 0x4800:
            t = ((base + i + 4) & ~3) + (h & 0xFF) * 4 - base
            for k in range(4):
                if 0 <= t + k < len(code):
                    wild[t + k] = 1
        if (h & 0xF800) == 0xF000 and i + 3 < len(code) and (struct.unpack_from('<H', code, i + 2)[0] & 0xF800) == 0xF800:
            wild[i:i + 4] = b'\1\1\1\1'
            i += 4
            continue
        i += 2
    return wild


class Matcher:
    def __init__(self):
        self.funcs, self.alls, self.names = payload_symbols()
        self.src = open(os.path.join(PAYLOAD, 'payload.gba'), 'rb').read()

    def func_bytes(self, name):
        for a, n in self.funcs:
            if n == name:
                nxt = [x for x in self.alls if x > a]
                end = nxt[0] if nxt else a + 0x40
                return a, self.src[a - BASE:end - BASE]
        raise KeyError(name)

    def find(self, img, name):
        a, code = self.func_bytes(name)
        wild = thumb_mask(code, a)
        rx = re.compile(b''.join(b'.' if w else re.escape(bytes([b])) for b, w in zip(code, wild)), re.DOTALL)
        hits = [BASE + m.start() for m in rx.finditer(img)]
        return hits

    def literal_names(self, name):
        """Symbol names of the literals in a payload function, by pool offset."""
        a, code = self.func_bytes(name)
        out = {}
        for i in range(0, len(code) - 1, 2):
            h = struct.unpack_from('<H', code, i)[0]
            if (h & 0xF800) == 0x4800:
                t = ((a + i + 4) & ~3) + (h & 0xFF) * 4 - a
                v = struct.unpack_from('<I', code, t)[0]
                out[t] = self.names.get(v, '0x%08X' % v)
        return out


def bl_sites(img, target):
    out = []
    for i in range(0, len(img) - 3, 2):
        h1, h2 = struct.unpack_from('<HH', img, i)
        if (h1 & 0xF800) == 0xF000 and (h2 & 0xF800) == 0xF800:
            off = ((h1 & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
            if off & 0x400000:
                off -= 0x800000
            if BASE + i + 4 + off == target:
                out.append(BASE + i)
    return out


def bl_target(img, site):
    h1, h2 = struct.unpack_from('<HH', img, site - BASE)
    off = ((h1 & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
    if off & 0x400000:
        off -= 0x800000
    return site + 4 + off


def func_start(img, addr):
    """Start of the Thumb function holding 'addr': the nearest push {.., lr} before it."""
    a = addr
    while a > BASE:
        a -= 2
        if (struct.unpack_from('<H', img, a - BASE)[0] & 0xFF00) == 0xB500:
            return a
    raise ValueError('no function start before %08X' % addr)


def pointer_refs(img, fn):
    return [BASE + m.start() for m in re.finditer(re.escape(struct.pack('<I', fn | 1)), img)]


def lit_loads(img, value):
    out = []
    for i in range(0, len(img) - 1, 2):
        h = struct.unpack_from('<H', img, i)[0]
        if (h & 0xF800) == 0x4800:
            a = ((BASE + i + 4) & ~3) + (h & 0xFF) * 4 - BASE
            if 0 <= a <= len(img) - 4 and struct.unpack_from('<I', img, a)[0] == value:
                out.append(BASE + i)
    return out


def check_memory(img):
    """Highest EWRAM address the program uses; refuses programs that might use
    the shim's area."""
    end_img = BASE + len(img)
    bss_end = None
    for r in lit_loads(img, end_img):
        h = struct.unpack_from('<H', img, r - BASE)[0]
        pool = ((r + 4) & ~3) + (h & 0xFF) * 4
        nxt = struct.unpack_from('<I', img, pool - BASE + 4)[0]
        if end_img < nxt < BASE + 0x40000:
            bss_end = nxt
    if bss_end is None:
        raise ValueError('end of .bss not found')
    for i in range(0, len(img) - 1, 2):
        h = struct.unpack_from('<H', img, i)[0]
        if (h & 0xF800) == 0x4800:
            a = ((BASE + i + 4) & ~3) + (h & 0xFF) * 4 - BASE
            if 0 <= a <= len(img) - 4:
                v = struct.unpack_from('<I', img, a)[0]
                if bss_end < v < BASE + 0x40000:
                    raise ValueError('uses EWRAM at %08X, past its .bss' % v)
    mem_end = bss_end + SAVE_BLOCKS_MAX
    if mem_end + 0x400 > SHIM_ADDR:
        raise ValueError('may use EWRAM up to %08X, too close to the shim' % mem_end)
    return mem_end


def find_save_link(img):
    """The link code's save step: SavePhase(0), wait for the GameCube, SavePhase(1),
    with JOY_TRANS written in between (Colosseum's sub_02005FCC). Returns
    (function, [site0, site1], SavePhase)."""
    found = []
    for i in range(0, len(img) - 3, 2):
        if struct.unpack_from('<H', img, i)[0] != 0x2000:          # movs r0, #0
            continue
        site0 = BASE + i + 2
        h1, h2 = struct.unpack_from('<HH', img, i + 2)
        if (h1 & 0xF800) != 0xF000 or (h2 & 0xF800) != 0xF800:
            continue
        t = bl_target(img, site0)
        for j in range(i + 6, min(i + 0x60, len(img) - 5), 2):
            if struct.unpack_from('<H', img, j)[0] == 0x2001:       # movs r0, #1
                h1, h2 = struct.unpack_from('<HH', img, j + 2)
                if (h1 & 0xF800) == 0xF000 and (h2 & 0xF800) == 0xF800 and bl_target(img, BASE + j + 2) == t:
                    window = img[i:j + 0x60]
                    if struct.pack('<I', JOY_TRANS) in window:
                        found.append((site0, BASE + j + 2, t))
                    break
    return found


def analyze(path, matcher):
    prog = open(path, 'rb').read()
    res = {'file': os.path.basename(path)}
    ld = find_loader(prog)
    if not ld:
        raise ValueError('no XD loader found')
    off, bstart, bend = ld
    res.update(loaderOffset=off, blobStart=bstart, blobEnd=bend)
    # XD overwrites the header's game code when it sends a program, so the hook
    # hashes only what runs: the compressed program, then the loader.
    if bstart & 3 or bend & 3 or off & 3:
        raise ValueError('unaligned blob or loader')
    res['recvHash'] = fnv_words(prog[off:off + LOADER_SIZE], fnv_words(prog[bstart:bend]))
    img, used = lz77_decode(prog, bstart)
    if used > bend:
        raise ValueError('compressed data runs past the blob')
    res['decSize'] = len(img) // 4 * 4
    res['decHash'] = fnv_words(img[HEADER_SIZE:res['decSize']])
    res['memEnd'] = check_memory(img)

    def one(name):
        hits = matcher.find(img, name)
        if len(hits) != 1:
            raise ValueError('%s: %d matches' % (name, len(hits)))
        return hits[0]

    f = {n: one(n) for n in ('IdentifyFlash', 'ReadFlash', 'VerifyFlashSector', 'VerifyFlashSectorNBytes',
                             'ProgramFlashSectorAndVerify', 'ProgramFlashSectorAndVerifyNBytes', 'ReadFlashId',
                             'SwitchFlashBank', 'm4aSoundVSyncOff', 'm4aSoundVSyncOn')}
    # variables, from IdentifyFlash's literal pool (same layout as the payload's)
    lit = matcher.literal_names('IdentifyFlash')
    want = {'ProgramFlashByte': 'programFlashByte', 'ProgramFlashSector': 'programFlashSector',
            'EraseFlashChip': 'eraseFlashChip', 'EraseFlashSector': 'eraseFlashSector',
            'WaitForFlashWrite': 'waitForFlashWrite', 'gFlashMaxTime': 'flashMaxTime', 'gFlash': 'flash'}
    var = {}
    for pool_off, sym in lit.items():
        if sym in want:
            var[want[sym]] = struct.unpack_from('<I', img, f['IdentifyFlash'] - BASE + pool_off)[0]
    if len(var) != len(want):
        raise ValueError('flash variables not all found: %s' % var)

    sites = {
        'identify': bl_sites(img, f['IdentifyFlash']),
        'read': bl_sites(img, f['ReadFlash']),
        'verify': bl_sites(img, f['VerifyFlashSector']),
        'verifyN': bl_sites(img, f['VerifyFlashSectorNBytes']),
    }
    # Nothing else may reach the chip directly.
    others = {n: bl_sites(img, f[n]) for n in ('ReadFlashId', 'SwitchFlashBank')}
    for n, s in others.items():
        # agb_flash, agb_flash_1m, agb_flash_mx and agb_flash_le sit together
        inside = [x for x in s if f['SwitchFlashBank'] <= x < f['SwitchFlashBank'] + FLASH_LIB_SIZE]
        if s != inside:
            raise ValueError('%s is called from outside the flash library: %s' % (n, s))
    if len(sites['identify']) != 1 or len(sites['verify']) != 1 or len(sites['verifyN']) != 1 or not sites['read']:
        raise ValueError('unexpected call sites: %s' % sites)
    if not (f['ProgramFlashSectorAndVerify'] < sites['verify'][0] < f['ProgramFlashSectorAndVerifyNBytes']):
        raise ValueError('VerifyFlashSector called from an unexpected place')
    if not (f['ProgramFlashSectorAndVerifyNBytes'] < sites['verifyN'][0] < f['IdentifyFlash']):
        raise ValueError('VerifyFlashSectorNBytes called from an unexpected place')
    # Every way to change the save must go through SavePhase, so that a commit
    # after each SavePhase call writes all changes to the cart.
    lib_lo, lib_hi = f['SwitchFlashBank'], f['SwitchFlashBank'] + FLASH_LIB_SIZE
    writers = set(bl_sites(img, f['ProgramFlashSectorAndVerify']) + bl_sites(img, f['ProgramFlashSectorAndVerifyNBytes']))
    for v in ('programFlashByte', 'programFlashSector', 'eraseFlashChip', 'eraseFlashSector'):
        writers |= set(x for x in lit_loads(img, var[v]) if not lib_lo <= x < lib_hi)
    def closure_upto(stop):
        """Functions that can reach a save write, walking callers up to 'stop'."""
        seen = set()
        todo = [func_start(img, x) for x in writers]
        while todo:
            fn = todo.pop()
            if fn in seen:
                continue
            seen.add(fn)
            if fn != stop:
                todo += [func_start(img, x) for x in bl_sites(img, fn)]
        return seen

    def sealed(closure, stop):
        """True when nothing outside 'closure' can enter it except through 'stop'."""
        for fn in closure - {stop}:
            if pointer_refs(img, fn):
                return False
            if any(func_start(img, x) not in closure for x in bl_sites(img, fn)):
                return False
        return True

    save_phase = None
    for s0, s1, t in find_save_link(img):
        c = closure_upto(t)
        if t in c and sealed(c, t) and sorted(bl_sites(img, t)) == sorted([s0, s1]):
            if save_phase is not None:
                raise ValueError('save step: more than one candidate')
            save_phase = t
            sites['savePhase'] = [s0, s1]
    if save_phase is None:
        if not sealed(closure_upto(None), None):
            raise ValueError('the save can be written without going through a save step')
        sites['savePhase'] = []
    res['canWrite'] = save_phase is not None
    res['functions'] = {
        'identifyFlash': f['IdentifyFlash'] | 1, 'readFlash': f['ReadFlash'] | 1,
        'verifyFlashSector': f['VerifyFlashSector'] | 1, 'verifyFlashSectorNBytes': f['VerifyFlashSectorNBytes'] | 1,
        'savePhase': (save_phase | 1) if save_phase else 0, 'soundVSyncOff': f['m4aSoundVSyncOff'] | 1, 'soundVSyncOn': f['m4aSoundVSyncOn'] | 1,
    }
    res['variables'] = var
    res['sites'] = sites
    res['siteBytes'] = {k: [img[s - BASE:s - BASE + 4].hex() for s in v] for k, v in sites.items()}
    return res, prog, img


def main(argv):
    m = Matcher()
    out = []
    for p in argv[1:]:
        try:
            r, prog, img = analyze(p, m)
        except ValueError as e:
            print('%s: not usable (%s)' % (os.path.basename(p), e))
            continue
        out.append(r)
        print('%s: loader 0x%X, image %d bytes, memory up to %08X, writes %s, IdentifyFlash %08X, SavePhase %08X, sites %s' % (
            r['file'], r['loaderOffset'], r['decSize'], r['memEnd'], r['canWrite'], r['functions']['identifyFlash'] & ~1,
            r['functions']['savePhase'] & ~1, {k: ['%08X' % x for x in v] for k, v in r['sites'].items()}))
    if out:
        json.dump(out, sys.stdout if False else open(os.devnull, 'w'))
    return out


if __name__ == '__main__':
    main(sys.argv)
