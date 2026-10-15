#!/usr/bin/env python3
"""Runs the flasher, built by make_flasher.py from a real Emerald dump, in an
ARM emulator against the simulated carts in sim_cart.c: every supported
board, power cuts at many points followed by a second run, lying CFI tables,
failing erases and programs, a cart that differs from the dump, and boards
that must be refused.

usage: flasher_emu_test.py ROM_DUMP [scenario ...]
"""
import ctypes
import os
import struct
import subprocess
import sys
import time

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_LR, UC_ARM_REG_PC

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.normpath(os.path.join(HERE, '..'))
ELF = os.path.join(TOOLS, 'flasher', 'build', 'flasher.elf')
sys.path.insert(0, TOOLS)
import make_flasher                     # noqa: E402

ROM_SIZE = 32 * 1024 * 1024

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
lib.SimSetCfiBlock.argtypes = [ctypes.c_uint32]
lib.SimLastErase.restype = ctypes.c_uint32
lib.SimEraseBlock.restype = ctypes.c_uint32
lib.SimLastViolation.restype = ctypes.c_char_p


class SimStats(ctypes.Structure):
    _fields_ = [('erases', ctypes.c_uint), ('programs', ctypes.c_uint), ('violations', ctypes.c_uint),
                ('strayWrites', ctypes.c_uint), ('badBankWrites', ctypes.c_uint)]


lib.SimStats.restype = ctypes.POINTER(SimStats)

BOARDS = {'plain': 0, 'intel_buffered': 1, 'intel_word': 2, 'amd_swapped': 3, 'amd_dual': 4,
          'amd_x16': 5, 'intel_unknown': 6}


def symbols(elf=ELF):
    out = subprocess.run(['arm-none-eabi-nm', elf], capture_output=True, text=True, check=True).stdout
    return {p[2]: int(p[0], 16) for p in (l.split() for l in out.splitlines()) if len(p) == 3}


SYMS = symbols()
failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print('    FAIL:', msg)
    return cond


def rom_view():
    return ctypes.cast(lib.SimRom(), ctypes.POINTER(ctypes.c_uint8 * ROM_SIZE)).contents


def stats():
    return lib.SimStats().contents


class Run:
    """One power-on of the GBA running the flasher."""

    def __init__(self, image, stop_after_ops=None, fail_program_at=(), fail_erase_at=(), max_errors=3, syms=None):
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(0x00000000, 0x4000)
        uc.mem_map(0x02000000, 0x40000)
        uc.mem_map(0x03000000, 0x8000)
        uc.mem_map(0x04000000, 0x1000)
        uc.mem_map(0x05000000, 0x400)
        uc.mem_map(0x06000000, 0x18000)
        uc.mem_map(0x07000000, 0x400)
        uc.mmio_map(0x08000000, 0x02000000, self.rom_read, None, self.rom_write, None)
        uc.mmio_map(0x0E000000, 0x10000, self.sram_read, None, self.sram_write, None)
        uc.mem_write(0x02000000, image)
        for name, fn in (('WaitVBlank', self.ret0), ('ReadKeys', self.read_keys),
                         ('Finish', self.finish), ('ErrorWait', self.error_wait)):
            addr = (syms or SYMS)[name] & ~1
            uc.hook_add(UC_HOOK_CODE, fn, begin=addr, end=addr)
        self.uc = uc
        self.keys_toggle = 0
        self.finished = None
        self.errors = []
        self.max_errors = max_errors
        self.stop_after_ops = stop_after_ops
        self.fail_program_at = set(fail_program_at)
        self.fail_erase_at = set(fail_erase_at)
        self.interrupted = False
        self.erased = []

    @staticmethod
    def ops():
        s = stats()
        return s.erases + s.programs

    # cart bus
    def rom_read(self, uc, offset, size, data):
        if size == 1:
            return (lib.HostRomRead16(offset & ~1) >> ((offset & 1) * 8)) & 0xFF
        if size == 2:
            return lib.HostRomRead16(offset)
        return lib.HostRomRead16(offset) | (lib.HostRomRead16(offset + 2) << 16)

    def rom_write(self, uc, offset, size, value, data):
        if offset == 0x01000000 and size == 1:
            lib.HostBankWrite8(value)
            return
        if size != 2:
            raise RuntimeError('unexpected %d-byte cart write at 0x%X' % (size, offset))
        before = stats().erases
        lib.HostRomWrite16(offset, value)
        if stats().erases != before:
            self.erased.append(lib.SimLastErase())
        n = self.ops()
        if n in self.fail_program_at:
            self.fail_program_at.discard(n)
            lib.SimFailNextProgram()
        if n in self.fail_erase_at:
            self.fail_erase_at.discard(n)
            lib.SimFailNextErase()
        if self.stop_after_ops is not None and n >= self.stop_after_ops and not self.interrupted:
            self.interrupted = True
            uc.emu_stop()

    def sram_read(self, uc, offset, size, data):
        assert size == 1, 'SRAM is 8-bit only'
        return lib.HostSramRead8(offset)

    def sram_write(self, uc, offset, size, value, data):
        assert size == 1, 'SRAM is 8-bit only'
        lib.HostSramWrite8(offset, value)

    # hooked functions
    def ret(self, uc, value):
        uc.reg_write(UC_ARM_REG_R0, value)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def ret0(self, uc, address, size, data):
        self.ret(uc, 0)

    def read_keys(self, uc, address, size, data):
        # Alternate "nothing held" with "L+R+A held": passes every key prompt.
        self.keys_toggle ^= 1
        self.ret(uc, 0x301 if self.keys_toggle == 0 else 0)

    def finish(self, uc, address, size, data):
        self.finished = uc.reg_read(UC_ARM_REG_R0)
        uc.emu_stop()

    def error_wait(self, uc, address, size, data):
        self.errors.append(uc.reg_read(UC_ARM_REG_R0))
        if len(self.errors) >= self.max_errors:
            uc.emu_stop()
        else:
            self.ret(uc, 0)

    def run(self):
        self.uc.emu_start(0x020000C0, 0xFFFFFFFF, timeout=900 * 1000000)
        return self

    def screen(self):
        vram = self.uc.mem_read(0x0600F800, 32 * 20 * 2)
        lines = []
        for y in range(20):
            row = struct.unpack_from('<30H', vram, y * 64)
            lines.append(''.join(chr((e & 0x3FF) + 0x20) if (e & 0x3FF) < 0x5F else '#' for e in row).rstrip())
        while lines and not lines[-1]:
            lines.pop()
        return '\n'.join(lines)


class Case:
    def __init__(self, dump, image, info, expected):
        self.dump, self.image, self.info, self.expected = dump, image, info, expected

    def setup(self, board, erase_block=None, cfi_block=None, seed=7, rom=None):
        lib.SimInit(BOARDS[board], seed)
        if erase_block:
            lib.SimSetEraseBlock(erase_block)
        if cfi_block:
            lib.SimSetCfiBlock(cfi_block)
        ctypes.memmove(lib.SimRom(), rom if rom is not None else self.dump, ROM_SIZE)

    def allowed_erase(self, start):
        info = self.info
        block = lib.SimEraseBlock()
        areas = [(lo, hi) for _, _, lo, hi in info['spans']] + [
            (info['staging'], info['staging'] + 0x40000), (info['test'] - 0x80000, info['test'] + 0x80000)]
        return any(a <= start and start + block <= b for a, b in areas)

    def common_checks(self, runs):
        s = stats()
        check(s.violations == 0, 'chip protocol violations: %d (%s)' % (s.violations, lib.SimLastViolation()))
        check(s.badBankWrites == 0, 'bank register writes with wrong values: %d' % s.badBankWrites)
        bad = [hex(e) for r in runs for e in r.erased if not self.allowed_erase(e)]
        check(not bad, 'erases outside the allowed areas: %s' % bad[:5])

    def rom_is(self, data):
        return bytes(rom_view()) == data


def first_diff(a, b):
    for i in range(0, len(a), 0x1000):
        if a[i:i + 0x1000] != b[i:i + 0x1000]:
            for j in range(i, i + 0x1000):
                if a[j] != b[j]:
                    return j
    return None


def scenario_ok(c, board, erase_block=None, cfi_block=None, label=None, **kw):
    print('  %s' % (label or board))
    c.setup(board, erase_block, cfi_block)
    t = time.time()
    r = Run(c.image, **kw).run()
    print('    %.1f s, %d erases, %d program ops, errors %s' % (time.time() - t, stats().erases, stats().programs,
                                                                ['%04X' % e for e in r.errors]))
    check(r.finished == 0, 'flasher did not finish (code %s, interrupted %s)\n%s' % (r.finished, r.interrupted, r.screen()))
    check('Done.' in r.screen(), 'no "Done." on screen:\n' + r.screen())
    rom = bytes(rom_view())
    d = first_diff(rom, c.expected)
    check(d is None, 'final ROM differs from the expected image at 0x%X' % (d or 0))
    c.common_checks([r])
    return r


def scenario_refused(c, board, message, rom=None, erase_block=None, cfi_block=None, label=None):
    print('  %s' % (label or board))
    c.setup(board, erase_block, cfi_block, rom=rom)
    before = bytes(rom_view())
    r = Run(c.image).run()
    check(r.finished == 1, 'expected a refusal (code %s)' % r.finished)
    check(message in r.screen().replace('\n', ' '), 'expected %r on screen:\n%s' % (message, r.screen()))
    check(bytes(rom_view()) == before, 'a refused run changed the cart ROM')
    check(stats().erases == 0 and stats().programs == 0, 'a refused run erased or programmed')
    return r


def scenario_interrupted(c, board, cut_points, erase_block=None):
    """Power cut after N flash operations, then a second complete run."""
    for n in cut_points:
        c.setup(board, erase_block)
        r1 = Run(c.image, stop_after_ops=n).run()
        if not r1.interrupted:
            check(False, 'cut at %d: the run ended before the cut' % n)
            continue
        lib.SimPowerCycle()
        r2 = Run(c.image).run()
        ok = check(r2.finished == 0 and 'Done.' in r2.screen(),
                   'cut at %d: second run did not finish (code %s)\n%s' % (n, r2.finished, r2.screen()))
        d = first_diff(bytes(rom_view()), c.expected)
        ok &= check(d is None, 'cut at %d: final ROM differs at 0x%X' % (n, d or 0))
        c.common_checks([r1, r2])
        print('    cut after %6d ops: %s' % (n, 'ok' if ok else 'FAILED'))


def main(argv):
    dump_path = argv[1]
    only = set(argv[2:])
    dump = open(dump_path, 'rb').read()
    dump = dump + b'\xff' * (ROM_SIZE - len(dump))
    patch = open(os.path.join(TOOLS, 'colosseum-mb-repro.bps'), 'rb').read()
    code = open(os.path.join(TOOLS, 'flasher', 'flasher_code.bin'), 'rb').read()
    hook = open(os.path.join(TOOLS, 'linkhook.bin'), 'rb').read()
    previous = [open(p, 'rb').read() for p in make_flasher.PREVIOUS_HOOKS]
    image, info = make_flasher.build(dump, patch, code, hook, previous)
    expected = info['target'] + dump[len(info['target']):]
    c = Case(dump, image, info, expected)
    print('flasher image %d bytes; writes %s; spare 0x%X, test 0x%X' % (
        len(image), ', '.join('0x%X-0x%X' % (d0, d1) for d0, d1, _, _ in info['spans']), info['staging'], info['test']))

    def want(name):
        return not only or name in only

    if want('boards'):
        print('complete runs')
        r = scenario_ok(c, 'amd_swapped')                       # 64 KiB blocks
        print('\n'.join('    | ' + l for l in r.screen().splitlines()))
        scenario_ok(c, 'amd_dual')                              # 2 x 64 KiB = 128 KiB bus blocks
        scenario_ok(c, 'intel_word')                            # 64 KiB, CFI with parameter blocks
        scenario_ok(c, 'intel_buffered', erase_block=0x20000, label='intel_buffered, 128 KiB blocks')
        scenario_ok(c, 'intel_buffered', erase_block=0x40000, label='intel_buffered, 256 KiB blocks')
        scenario_ok(c, 'amd_swapped', erase_block=0x20000, label='amd_swapped, 128 KiB blocks')

    if want('again'):
        print('second run on a finished cart')
        c.setup('amd_swapped')
        Run(c.image).run()
        lib.SimPowerCycle()
        s0 = (stats().erases, stats().programs)
        r = Run(c.image).run()
        check(r.finished == 0 and 'Already written' in r.screen(), 'second run:\n' + r.screen())
        check((stats().erases, stats().programs) == s0, 'second run wrote to the cart')
        check(bytes(rom_view()) == expected, 'second run changed the ROM')

    if want('v02'):
        print('cart flashed with v0.2 (Colosseum program only) before')
        v02 = info['colosseum'] + dump[len(info['colosseum']):]
        for board, eb in (('amd_swapped', 0x20000), ('intel_buffered', 0x40000), ('amd_swapped', None)):
            c.setup(board, eb, rom=v02)
            r = Run(c.image).run()
            d = first_diff(bytes(rom_view()), c.expected)
            erased = sorted(set(r.erased))
            check(r.finished == 0 and d is None, '%s: code %s, ROM differs at %s\n%s' % (board, r.finished, d, r.screen()))
            c.common_checks([r])
            print('    %s, %s blocks: erased %s' % (board, '%d KiB' % (lib.SimEraseBlock() >> 10), ' '.join('%X' % e for e in erased)))
        print('\n'.join('    | ' + l for l in r.screen().splitlines()))
        # the block of the program area already written by v0.2 is left alone (128 KiB blocks)
        c.setup('amd_swapped', 0x20000, rom=v02)
        r = Run(c.image).run()
        check(0x9A0000 not in r.erased, 'the block v0.2 already wrote was erased again')

    if want('v05'):
        print('cart flashed with v0.5 before (old XD hook)')
        v05 = info['earlier'][0] + dump[len(info['earlier'][0]):]
        for board, eb in (('amd_swapped', 0x20000), ('intel_buffered', 0x40000), ('amd_swapped', None)):
            c.setup(board, eb, rom=v05)
            r = Run(c.image).run()
            d = first_diff(bytes(rom_view()), c.expected)
            erased = sorted(set(r.erased))
            check(r.finished == 0 and d is None, '%s: code %s, ROM differs at %s\n%s' % (board, r.finished, d, r.screen()))
            c.common_checks([r])
            game = [e for e in erased if not (info['staging'] <= e < info['staging'] + 0x40000 or info['test'] - 0x80000 <= e < info['test'] + 0x80000)]
            check(all(0x9C0000 <= e < 0xA00000 for e in game), '%s: erased game blocks %s' % (board, game))
            print('    %s, %d KiB blocks: game blocks erased %s' % (board, lib.SimEraseBlock() >> 10, ' '.join('%X' % e for e in game)))
        print('\n'.join('    | ' + l for l in r.screen().splitlines()))
        print('  power cuts while updating a v0.5 cart (amd_swapped, 128 KiB blocks)')
        c.setup('amd_swapped', 0x20000, rom=v05)
        Run(c.image).run()
        total = Run.ops()
        for n in sorted(set([1, 2, 3, 5, 8] + [total * k // 12 for k in range(1, 12)] + [total - 1])):
            c.setup('amd_swapped', 0x20000, rom=v05)
            r1 = Run(c.image, stop_after_ops=n).run()
            lib.SimPowerCycle()
            r2 = Run(c.image).run()
            d = first_diff(bytes(rom_view()), c.expected)
            ok = check(r1.interrupted and r2.finished == 0 and d is None, 'cut at %d: code %s, ROM differs at %s\n%s' % (n, r2.finished, d, r2.screen()))
            c.common_checks([r1, r2])
            print('    cut after %5d of %d ops: %s' % (n, total, 'ok' if ok else 'FAILED'))

    if want('save'):
        print('save area changed since the dump (the user kept playing)')
        played = bytearray(dump)
        for off in range(0xFC0000, 0xFE0000, 0x1000):
            played[off:off + 16] = bytes((off >> 12) + i & 0xFF for i in range(16))
        c.setup('intel_buffered', 0x40000, rom=bytes(played))
        r = Run(c.image).run()
        want_rom = bytearray(played)
        for d0, d1, _, _ in info['spans']:
            want_rom[d0:d1] = expected[d0:d1]
        d = first_diff(bytes(rom_view()), bytes(want_rom))
        check(r.finished == 0 and d is None, 'code %s, ROM differs at %s' % (r.finished, d))
        c.common_checks([r])

    if want('faults'):
        print('failing erases and programs')
        scenario_ok(c, 'amd_swapped', fail_program_at=[200], fail_erase_at=[3], label='amd_swapped, one failed program, one failed erase')
        scenario_ok(c, 'intel_buffered', erase_block=0x40000, fail_program_at=[300, 301], label='intel_buffered, two failed programs in a row')
        r = scenario_ok(c, 'intel_word', fail_program_at=[1000, 1500], label='intel_word, two failed copies (error screen, A to retry)')
        check(len(r.errors) >= 1, 'expected the error screen at least once')

    if want('refuse'):
        print('refusals')
        print('  intel_word, a chip that never programs')
        c.setup('intel_word')
        r = Run(c.image, fail_program_at=[0, 1, 2, 3]).run()
        check(r.finished == 1 and 'erase test failed' in r.screen() and 'Game data is intact' in r.screen(),
              'a chip that cannot program should stop in the erase test:\n' + r.screen())
        check(bytes(rom_view())[:info['staging']] == dump[:info['staging']], 'game data changed')
        scenario_refused(c, 'amd_x16', 'not supported')
        scenario_refused(c, 'plain', 'not supported', label='plain ROM (unknown chip)')
        scenario_refused(c, 'intel_unknown', 'not supported')
        other = bytearray(dump)
        other[0x123456] ^= 0x01
        scenario_refused(c, 'amd_swapped', 'only writes the cart it was built for', rom=bytes(other), label='cart differs from the dump')
        scenario_refused(c, 'intel_buffered', 'layout is not supported', erase_block=0x80000, label='512 KiB blocks')

    if want('cfi'):
        print('CFI tables that do not match the chip')
        for real, said in ((0x20000, 0x10000), (0x40000, 0x20000), (0x10000, 0x20000), (0x20000, 0x40000)):
            board = 'intel_buffered' if real >= 0x20000 and said >= 0x20000 else 'amd_swapped'
            print('  %s: real %d KiB, CFI says %d KiB' % (board, real >> 10, said >> 10))
            c.setup(board, real, said)
            r = Run(c.image).run()
            # Smaller blocks than reported: on Intel the marker beyond the real
            # block is refused (block still locked), which also stops the test.
            msgs = ['larger'] if real > said else ['smaller', 'erase test failed']
            screen = r.screen()
            check(r.finished == 1 and any(m in screen for m in msgs) and 'Game data is intact' in screen,
                  'expected a stop with %r:\n%s' % (msgs, screen))
            rom = bytes(rom_view())
            outside = rom[:info['staging']] + rom[info['test'] + 0x80000:]
            check(outside == dump[:info['staging']] + dump[info['test'] + 0x80000:], 'ROM changed outside the spare area')
            bad = [hex(e) for e in r.erased if not c.allowed_erase(e)]
            check(not bad, 'erases outside the allowed areas: %s' % bad[:5])

    if want('cuts'):
        print('power cuts, then a second run')
        c.setup('amd_swapped')
        Run(c.image).run()
        total = Run.ops()
        print('  amd_swapped (64 KiB blocks): %d flash operations in a full run' % total)
        points = sorted(set([1, 2, 3, 5, 8, 12] + [total * k // 15 for k in range(1, 15)] + [total - 2, total - 1]))
        scenario_interrupted(c, 'amd_swapped', points)
        c.setup('intel_buffered', 0x40000)
        Run(c.image).run()
        total = Run.ops()
        print('  intel_buffered (256 KiB blocks): %d flash operations in a full run' % total)
        points = sorted(set([1, 3, 6, 9] + [total * k // 13 for k in range(1, 13)] + [total - 1]))
        scenario_interrupted(c, 'intel_buffered', points, erase_block=0x40000)

    if want('cuts2'):
        print('two power cuts in a row')
        c.setup('amd_swapped')
        Run(c.image).run()
        total = Run.ops()
        for a, b in ((total // 5, total // 6), (total // 2, 5), (total * 3 // 4, total // 10), (7, total // 3)):
            c.setup('amd_swapped')
            Run(c.image, stop_after_ops=a).run()
            lib.SimPowerCycle()
            Run(c.image, stop_after_ops=Run.ops() + b).run()
            lib.SimPowerCycle()
            r = Run(c.image).run()
            d = first_diff(bytes(rom_view()), c.expected)
            ok = check(r.finished == 0 and d is None, 'cuts at %d then +%d: code %s, ROM differs at %s' % (a, b, r.finished, d))
            print('    cuts after %d, then %d more: %s' % (a, b, 'ok' if ok else 'FAILED'))

    print('%d FAILURES' % failures if failures else 'all flasher tests passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
