#!/usr/bin/env python3
"""End-to-end emulator test of the XD link fix on an Emerald dump.

Phase 1, the hook: the patched copyright-screen code of Emerald runs as if a
program had just arrived from the GameCube. LinkHook prepares XD's program,
Emerald starts it, XD's stub and loader decompress it and XdPostLoad installs
the shim. Stops where XD's decompressed program starts. Also covers
Colosseum's swap, programs that must be left alone, and a program that
changes after LinkHook checked it.

Phase 2, the save: from that state, XD's own start-up code runs up to its save
read, against simulated repro carts holding the dump's save; then the trade
program's link save step runs with the GameCube handshake emulated. The save
in cart ROM is checked afterwards and read again after a power cycle.

usage: xd_emu_test.py EMERALD_DUMP XD_PROGRAM_DIR
  XD_PROGRAM_DIR: the folder written by patch_colosseum_iso.py extract
"""
import ctypes
import glob
import os
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_INTR, UC_HOOK_MEM_WRITE, UcError
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP,
                               UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR)

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(TOOLS, 'xdlink'))
import bps                                      # noqa: E402
import analyze_xd                               # noqa: E402
import emerald_hook                             # noqa: E402
from emerald_rom import PROGRAM_SIZE, find_program   # noqa: E402

ROM_SIZE = 0x2000000
SAVE = 0xFC0000
SECTOR = 0x1000
SIG = 0x08012025
GCMB_STRUCT = 0x030062B0      # Emerald's gMultibootProgramStruct
INTRO_CASE = 0x0816CE44       # SetUpCopyrightScreen, case 141, after the fade
SHIM = 0x0203E800
STOP = 0x03007F00

lib = ctypes.CDLL(os.path.join(HERE, 'libsimcart.so'))
lib.HostRomRead16.restype = ctypes.c_uint16
lib.HostRomRead16.argtypes = [ctypes.c_uint32]
lib.HostRomWrite16.argtypes = [ctypes.c_uint32, ctypes.c_uint16]
lib.HostSramRead8.restype = ctypes.c_uint8
lib.HostSramRead8.argtypes = [ctypes.c_uint32]
lib.HostSramWrite8.argtypes = [ctypes.c_uint32, ctypes.c_uint8]
lib.HostBankWrite8.argtypes = [ctypes.c_uint8]
lib.SimInit.argtypes = [ctypes.c_int, ctypes.c_uint32]
lib.SimRom.restype = ctypes.POINTER(ctypes.c_uint8)
lib.SimSetEraseBlock.argtypes = [ctypes.c_uint32]


class SimStats(ctypes.Structure):
    _fields_ = [('erases', ctypes.c_uint), ('programs', ctypes.c_uint), ('violations', ctypes.c_uint),
                ('strayWrites', ctypes.c_uint), ('badBankWrites', ctypes.c_uint)]


lib.SimStats.restype = ctypes.POINTER(SimStats)
lib.SimLastViolation.restype = ctypes.c_char_p
BOARDS = {'amd_swapped': 3, 'amd_dual': 4, 'intel_buffered': 1, 'intel_word': 2, 'amd_x16': 5}

failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print('    FAIL:', msg)


def sim_rom():
    return ctypes.cast(lib.SimRom(), ctypes.POINTER(ctypes.c_uint8 * ROM_SIZE)).contents


def build_rom(dump):
    rom = bytearray(dump)
    pos = find_program(rom)
    new = bps.apply(bytes(rom[pos:pos + PROGRAM_SIZE]), open(os.path.join(TOOLS, 'colosseum-mb-repro.bps'), 'rb').read())
    rom[pos:pos + len(new)] = new
    hook = open(os.path.join(TOOLS, 'linkhook.bin'), 'rb').read()
    return bytes(emerald_hook.apply(rom, hook)), hook


def slots(rom):
    out = []
    for slot in range(2):
        ids, counters = set(), set()
        for i in range(14):
            off = SAVE + (slot * 14 + i) * SECTOR
            cid, _, sig, counter = struct.unpack_from('<HHII', bytes(rom[off + 0xFF4:off + 0x1000]))
            if sig == SIG:
                ids.add(cid)
                counters.add(counter)
        out.append(counters.pop() if ids == set(range(14)) and len(counters) == 1 else None)
    return out


def newest_sector(rom, chunk):
    s = slots(rom)
    slot = 0 if (s[0] or -1) > (s[1] or -1) else 1
    for i in range(14):
        off = SAVE + (slot * 14 + i) * SECTOR
        if struct.unpack_from('<H', bytes(rom[off + 0xFF4:off + 0xFF6]))[0] == chunk:
            return bytes(rom[off:off + SECTOR])
    raise ValueError('chunk %d not found' % chunk)


class Gba:
    """GBA memory map in Unicorn. The cart is either plain memory (phase 1, code
    runs from ROM) or the simulated flash cart (phase 2, ROM through callbacks)."""

    def __init__(self, rom=None):
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(0x00000000, 0x4000)
        uc.mem_map(0x02000000, 0x40000)
        uc.mem_map(0x03000000, 0x8000)
        uc.mem_map(0x04000000, 0x1000)
        uc.mem_map(0x05000000, 0x400)
        uc.mem_map(0x06000000, 0x18000)
        uc.mem_map(0x07000000, 0x400)
        if rom is not None:
            uc.mem_map(0x08000000, ROM_SIZE)
            uc.mem_write(0x08000000, rom)
            uc.mem_map(0x0E000000, 0x10000)
        else:
            uc.mmio_map(0x08000000, ROM_SIZE, self.rom_read, None, self.rom_write, None)
            uc.mmio_map(0x0E000000, 0x10000, self.sram_read, None, self.sram_write, None)
        uc.mem_write(0x04000130, struct.pack('<H', 0x3FF))   # no keys held
        uc.mem_write(STOP, struct.pack('<H', 0xE7FE))
        uc.hook_add(UC_HOOK_INTR, self.swi)
        uc.hook_add(UC_HOOK_MEM_WRITE, self.dma_write, begin=0x040000DC, end=0x040000DF)
        uc.hook_add(UC_HOOK_MEM_WRITE, self.joy_write, begin=0x04000154, end=0x04000157)
        self.uc = uc
        self.joy_flags = None
        self.gc_acks = 0

    def rom_read(self, uc, offset, size, data):
        if size == 1:
            return (lib.HostRomRead16(offset & ~1) >> ((offset & 1) * 8)) & 0xFF
        if size == 2:
            return lib.HostRomRead16(offset)
        return lib.HostRomRead16(offset) | (lib.HostRomRead16(offset + 2) << 16)

    def rom_write(self, uc, offset, size, value, data):
        if offset == 0x01000000 and size == 1:
            lib.HostBankWrite8(value)
        elif size == 2:
            lib.HostRomWrite16(offset, value)
        else:
            raise RuntimeError('unexpected %d-byte cart write at 0x%X' % (size, offset))

    def sram_read(self, uc, offset, size, data):
        if size != 1:
            raise RuntimeError('SRAM is 8-bit only (read size %d at 0x%X)' % (size, offset))
        return lib.HostSramRead8(offset)

    def sram_write(self, uc, offset, size, value, data):
        if size != 1:
            raise RuntimeError('SRAM is 8-bit only (write size %d at 0x%X)' % (size, offset))
        lib.HostSramWrite8(offset, value)

    def dma_write(self, uc, access, address, size, value, data):
        """DMA3 with immediate start, as XD's stub uses it to copy its loader."""
        if address == 0x040000DC and size == 4:
            cnt = value
        elif address == 0x040000DE and size == 2:
            cnt = (value << 16) | struct.unpack('<H', uc.mem_read(0x040000DC, 2))[0]
        else:
            return
        if not cnt & 0x80000000:
            return
        if (cnt >> 28) & 3:
            raise RuntimeError('only immediate DMA is emulated')
        src, dst = struct.unpack('<II', uc.mem_read(0x040000D4, 8))
        n = cnt & 0xFFFF or 0x10000
        width = 4 if cnt & (1 << 26) else 2
        uc.mem_write(dst, bytes(uc.mem_read(src, n * width)))

    def joy_write(self, uc, access, address, size, value, data):
        """GameCube side of the save handshake: after phase 0 (JOY_TRANS = 0),
        say go ahead (flag bit 2) with no command pending (bit 3)."""
        if self.joy_flags is None:
            return
        b = uc.mem_read(self.joy_flags, 1)[0]
        if value == 0 and not (b & 4) and self.gc_acks == 0:
            self.gc_acks += 1
            uc.mem_write(self.joy_flags, bytes([(b | 4) & ~8]))

    def swi(self, uc, intno, data):
        pc = uc.reg_read(UC_ARM_REG_PC)
        thumb = uc.reg_read(UC_ARM_REG_CPSR) & 0x20
        insn = pc - (2 if thumb else 4)
        num = uc.mem_read(insn, 1)[0] if thumb else uc.mem_read(insn + 2, 1)[0]
        r0, r1, r2 = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
        if num in (0x01, 0x02, 0x04, 0x05):     # RegisterRamReset, Halt, IntrWait, VBlankIntrWait
            return
        if num == 0x06:
            q = int(r0 if r0 < 0x80000000 else r0 - (1 << 32)) // int(r1 if r1 < 0x80000000 else r1 - (1 << 32))
            uc.reg_write(UC_ARM_REG_R0, q & 0xFFFFFFFF)
            return
        if num in (0x0B, 0x0C):
            count = r2 & 0x1FFFFF
            word = 4 if (num == 0x0C or r2 & (1 << 26)) else 2
            if r2 & (1 << 24):
                uc.mem_write(r1, bytes(uc.mem_read(r0, word)) * count)
            else:
                uc.mem_write(r1, bytes(uc.mem_read(r0, count * word)))
            return
        raise RuntimeError('unhandled SWI 0x%X at 0x%X' % (num, insn))

    def run(self, start, until, sp=0x03007E00, lr=None, args=(), limit=100_000_000):
        uc = self.uc
        for reg, val in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), args):
            uc.reg_write(reg, val)
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_LR, lr if lr is not None else STOP | 1)
        uc.emu_start(start, until, count=limit)
        pc = uc.reg_read(UC_ARM_REG_PC)
        if pc != until:
            raise RuntimeError('stopped at 0x%08X, not 0x%08X' % (pc, until))
        return uc.reg_read(UC_ARM_REG_R0)

    def read(self, addr, n):
        return bytes(self.uc.mem_read(addr, n))

    def u32(self, addr):
        return struct.unpack('<I', self.read(addr, 4))[0]


def receive(rom, program):
    """Phase 1: Emerald has just received 'program'. Runs Emerald's own code from
    the copyright screen until the program would start."""
    gba = Gba(rom)
    gba.uc.mem_write(0x02000000, program)
    gba.uc.mem_write(GCMB_STRUCT, bytes(0x2C))
    gba.uc.mem_write(GCMB_STRUCT + 2, b'\x02')       # image ready to boot
    return gba


def bl_target(word_bytes, site):
    h1, h2 = struct.unpack('<HH', word_bytes)
    if (h1 & 0xF800) != 0xF000 or (h2 & 0xF800) != 0xF800:
        return None
    off = ((h1 & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
    if off & 0x400000:
        off -= 0x800000
    return site + 4 + off


def shim_symbols():
    syms = {}
    for line in open(os.path.join(TOOLS, 'xdlink', 'build', 'shim_syms.h')):
        p = line.split()
        syms[p[1][5:]] = int(p[2], 16)
    return syms


# XD overwrites the game code in the program's header ("TEST" on the disc)
# when it sends a program; its loader keeps the value. Any value will do here.
GC_CODE = b'\x1D\xC3\x5A\x01'


def phase1(rom, hook, prog_path, matcher, tweak=False, code=GC_CODE):
    name = os.path.basename(prog_path)
    program = bytearray(open(prog_path, 'rb').read())
    if code is not None:
        program[0xAC:0xB0] = code
        name += ' (game code %s)' % ('as on disc' if code == b'TEST' else 'set by XD')
    program = bytes(program)
    if tweak:
        # As if the GameCube changed a header byte and sent different padding
        # after the loader: the hook only looks at what the program needs.
        p = bytearray(program)
        p[0xBE] ^= 0x5A
        info0 = analyze_xd.analyze(prog_path, matcher)[0]
        end = info0['loaderOffset'] + 0x1C4
        p[end:] = bytes(len(p) - end) + b'\xAA' * 8
        program = bytes(p)
        name += ' (header byte and padding changed)'
    info, _, image = analyze_xd.analyze(prog_path, matcher)
    syms = shim_symbols()
    gba = receive(rom, program)
    gba.run(INTRO_CASE | 1, 0x02000000)
    ewram = gba.read(0x02000000, 0x40000)
    shim = open(os.path.join(TOOLS, 'xdlink', 'build', 'shim.bin'), 'rb').read()
    check(ewram[SHIM - 0x02000000:SHIM - 0x02000000 + len(shim)] == shim, '%s: shim not installed' % name)
    calls = {'identify': 'Xd_IdentifyFlash', 'read': 'Xd_ReadFlash', 'verify': 'Xd_VerifyFlashSector',
             'verifyN': 'Xd_VerifyFlashSectorNBytes', 'savePhase': 'Xd_SavePhase'}
    expect = bytearray(image)
    expect[0xAC:0xB0] = program[0xAC:0xB0]         # what the loader keeps
    for kind, sites in info['sites'].items():
        for s in sites:
            got = bl_target(ewram[s - 0x02000000:s - 0x02000000 + 4], s)
            check(got == syms[calls[kind]], '%s: %s call at %08X goes to %s' % (name, kind, s, got and hex(got)))
            expect[s - 0x02000000:s - 0x02000000 + 4] = ewram[s - 0x02000000:s - 0x02000000 + 4]
    check(ewram[:len(expect)] == bytes(expect), '%s: program differs from XD\'s beyond the redirected calls' % name)
    params = struct.unpack_from('<14I', ewram, syms['gXdParams'] - 0x02000000)
    want = [info['functions'][k] for k in ('identifyFlash', 'readFlash', 'verifyFlashSector', 'verifyFlashSectorNBytes',
                                           'savePhase', 'soundVSyncOff', 'soundVSyncOn')]
    want += [info['variables'][k] for k in ('programFlashByte', 'programFlashSector', 'eraseFlashChip', 'eraseFlashSector',
                                            'waitForFlashWrite', 'flashMaxTime', 'flash')]
    check(list(params) == want, '%s: shim parameters wrong' % name)
    check(gba.u32(0x030001C0) == 0, '%s: program index left behind' % name)
    print('  %s: hook and loader ok (%d calls redirected)' % (name, sum(len(v) for v in info['sites'].values())))
    return gba, info


def phase1_others(rom, xd_dir, matcher):
    # Colosseum: the swap Emerald always did.
    fake = bytearray(open(glob.glob(os.path.join(xd_dir, '*maxcng_english*'))[0], 'rb').read())
    fake[0xAC:0xB0] = b'Gc6e'
    gba = receive(rom, bytes(fake))
    gba.run(INTRO_CASE | 1, 0x020000C0)
    pos = emerald_hook.PROGRAM_AT
    want = bytearray(rom[pos:pos + PROGRAM_SIZE])
    want[0xAC:0xB0] = b'Gc6e'
    check(gba.read(0x02000000, PROGRAM_SIZE) == bytes(want), 'Colosseum: program not swapped in as before')
    print('  Colosseum program: swapped in as before')

    # Unknown programs run as sent: coupon.bin, and an XD program with one byte changed.
    for label, data in (('coupon.bin', open(glob.glob(os.path.join(xd_dir, '*coupon*'))[0], 'rb').read()),
                        ('changed XD program', None)):
        if data is None:
            p = bytearray(open(glob.glob(os.path.join(xd_dir, '*maxcng_english*'))[0], 'rb').read())
            p[0x5000] ^= 1
            data = bytes(p)
        gba = receive(rom, data)
        gba.run(INTRO_CASE | 1, 0x020000C0)
        check(gba.read(0x02000000, len(data)) == data, '%s: changed before running' % label)
        print('  %s: left alone' % label)

    # A program that matches when received but not after decompression (here,
    # changed between the check and the start) runs as it is, without the shim.
    path = glob.glob(os.path.join(xd_dir, '*maxcng_english*'))[0]
    data = open(path, 'rb').read()
    gba = receive(rom, data)
    info = analyze_xd.analyze(path, matcher)[0]
    # run up to the stub's entry, then damage the compressed data
    gba.run(INTRO_CASE | 1, 0x020000C0)
    gba.uc.mem_write(0x02000000 + info['blobStart'] + 0x100, b'\x00')
    gba.run(0x020000C0, 0x02000000)
    check(gba.read(SHIM, 16) != open(os.path.join(TOOLS, 'xdlink', 'build', 'shim.bin'), 'rb').read(16),
          'shim installed for a program that changed')
    check(gba.u32(0x030001C0) == 0, 'program index left behind after a mismatch')
    print('  program changed after the check: started without the shim')


def find_bytes(gba, needle):
    ew = gba.read(0x02000000, 0x40000)
    i = ew.find(needle)
    return None if i < 0 else 0x02000000 + i


def phase2(state, info, board, rom):
    """XD's start-up up to its save read, from the end of phase 1."""
    board, _, block = board.partition('/')
    lib.SimInit(BOARDS[board], 7)
    if block:
        lib.SimSetEraseBlock(int(block, 16))
    ctypes.memmove(lib.SimRom(), rom, ROM_SIZE)
    return boot_from(state, info)


def boot_from(state, info):
    gba = Gba(None)
    gba.uc.mem_write(0x02000000, state['ewram'])
    gba.uc.mem_write(0x03000000, state['iwram'])
    gba.uc.mem_write(0x04000000, state['io'])
    gba.uc.mem_write(0x04000130, struct.pack('<H', 0x3FF))
    gba.uc.mem_write(STOP, struct.pack('<H', 0xE7FE))
    # main() up to the instruction after its save read (maxcng: 0x02000290 strb r0,[r4])
    gba.run(0x02000000, state['after_read'], limit=200_000_000)
    return gba


def save_test(state, info, board, rom, writable):
    print('  board %s:' % board)
    syms = shim_symbols()
    gba = phase2(state, info, board, rom)
    before = bytes(sim_rom())
    sb2 = newest_sector(before, 0)[:0x20]
    at = find_bytes(gba, sb2)
    check(at is not None, 'save not read into memory')
    check(bytes(sim_rom()) == before, 'reading changed the cart')
    if at is None:
        return
    # Change the trainer name and run the trade program's link save step.
    gba.uc.mem_write(at, b'\xCE\xBF\xCD\xCE\xFF')        # "TEST"
    gba.joy_flags = state['joy_flags']
    # On a board whose writes are refused, XD retries every byte of each
    # sector before it gives up, which takes a while.
    failed = gba.run(state['link_save'] | 1, STOP, lr=STOP | 1, limit=100_000_000 if writable else 600_000_000)
    stats = lib.SimStats().contents
    after = bytes(sim_rom())
    print('    link save -> %d, erases %d, program ops %d, slots %s -> %s' % (
        failed, stats.erases, stats.programs, slots(before), slots(after)))
    check(stats.violations == 0, 'chip violations: %s' % lib.SimLastViolation())
    check(stats.badBankWrites == 0, 'bad bank register writes')
    check(after[:SAVE] == before[:SAVE] and after[SAVE + 0x20000:] == before[SAVE + 0x20000:],
          'cart changed outside the save area')
    if not writable:
        check(failed == 1 and after == before, 'save on an unsupported board was not refused cleanly')
        return
    old = slots(before)
    newest = max(c for c in old if c is not None)
    check(failed == 0 and gba.gc_acks == 1, 'link save failed')
    check(sorted(c for c in slots(after) if c is not None) == [newest, newest + 1], 'slots after save: %s' % slots(after))
    check(newest_sector(after, 0)[:5] == b'\xCE\xBF\xCD\xCE\xFF', 'new trainer name not in the newest slot')

    # Power cycle, start XD's program again, read the save back.
    lib.SimPowerCycle()
    gba2 = boot_from(state, info)
    check(find_bytes(gba2, b'\xCE\xBF\xCD\xCE\xFF' + sb2[5:0x20]) is not None, 'changed save not read back after power cycle')


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    dump = open(argv[1], 'rb').read()
    rom, hook = build_rom(dump)
    m = analyze_xd.Matcher()
    print('phase 1: Emerald receives programs')
    progs = sorted(glob.glob(os.path.join(argv[2], '*max*english*.gba')))
    states = {}
    for p in progs:
        gba, info = phase1(rom, hook, p, m)
        states[os.path.basename(p)] = (gba, info)
        phase1(rom, hook, p, m, code=b'TEST')
    phase1(rom, hook, [p for p in progs if 'maxcng' in p][0], m, tweak=True)
    phase1_others(rom, argv[2], m)

    print('phase 2: save on simulated carts (trade program)')
    gba, info = [v for k, v in states.items() if 'maxcng' in k][0]
    state = {'ewram': gba.read(0x02000000, 0x40000), 'iwram': gba.read(0x03000000, 0x8000),
             'io': gba.read(0x04000000, 0x1000), 'after_read': 0x02000292,
             'link_save': 0x02000AC4, 'joy_flags': 0x0201F240 + 0x87C}
    check(info['sites']['savePhase'] == [0x02000AC8, 0x02000B00], 'the test assumes the US trade program')
    for board, writable in (('amd_swapped', True), ('amd_swapped/20000', True), ('amd_dual', True), ('intel_buffered', True),
                            ('intel_word', True), ('amd_x16', False)):
        save_test(state, info, board, rom, writable)

    print('phase 2: battle program reads the save')
    gba, info = [v for k, v in states.items() if 'maxbtl' in k][0]
    lib.SimInit(BOARDS['amd_swapped'], 3)
    ctypes.memmove(lib.SimRom(), rom, ROM_SIZE)
    before = bytes(sim_rom())
    g = Gba(None)
    g.uc.mem_write(0x02000000, gba.read(0x02000000, 0x40000))
    g.uc.mem_write(0x03000000, gba.read(0x03000000, 0x8000))
    g.uc.mem_write(0x04000000, gba.read(0x04000000, 0x1000))
    g.uc.mem_write(0x04000130, struct.pack('<H', 0x3FF))
    g.uc.reg_write(UC_ARM_REG_SP, 0x03007E00)
    g.uc.emu_start(0x02000000, 0xFFFFFFF0, count=80_000_000)   # start-up; then it waits for the GameCube
    syms = shim_symbols()
    check(g.read(syms['gReproActive'], 1) == b'\x01', 'battle program: repro mode not active')
    check(g.u32(syms['gReproSaveOffset']) == SAVE, 'battle program: save offset %X' % g.u32(syms['gReproSaveOffset']))
    check(find_bytes(g, newest_sector(before, 0)[:0x20]) is not None, 'battle program: save not read')
    check(bytes(sim_rom()) == before and lib.SimStats().contents.erases == 0, 'battle program changed the cart')
    print('  battle program: save read from cart ROM, nothing written')

    print('%d FAILURES' % failures if failures else 'all XD emulator tests passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
