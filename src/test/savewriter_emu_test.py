#!/usr/bin/env python3
"""Runs the save writer, built by make_save_writer.py from a real Emerald dump,
in the ARM emulator against the simulated carts (see flasher_emu_test.py):
every supported board, power cuts followed by a second run, a cart already
flashed with the link program, a second run on a written cart, failures and
refusals. The save written is the dump's own save with the trainer name
changed and the checksum fixed, as PKHeX would leave it.

usage: savewriter_emu_test.py ROM_DUMP
"""
import ctypes
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import flasher_emu_test as fe           # noqa: E402
import gen3save                         # noqa: E402  (repro_tools is on the path via fe)
import make_save_writer                 # noqa: E402
import make_flasher                     # noqa: E402

SW_SYMS = fe.symbols(os.path.join(fe.TOOLS, 'flasher', 'build', 'savewriter.elf'))
check = fe.check


def edited_save(save):
    """Renames the trainer in the slot Emerald loads and fixes the checksum."""
    save = bytearray(save)
    _, _, sb2 = gen3save.newest(save)
    save[sb2:sb2 + 7] = bytes([0xCE, 0xBF, 0xCD, 0xCE, 0xBF, 0xCC, 0xFF])      # "TESTER"
    struct.pack_into('<H', save, sb2 + 0xFF6, gen3save.checksum(save[sb2:sb2 + gen3save.EMERALD_SIZES[0]]))
    return bytes(save)


class SaveCase:
    def __init__(self, dump, image, info, new_save):
        self.dump, self.image, self.info = dump, image, info
        s = info['save']
        self.expected = dump[:s] + new_save + dump[s + len(new_save):]

    def setup(self, board, erase_block=None, cfi_block=None, rom=None):
        fe.lib.SimInit(fe.BOARDS[board], 11)
        if erase_block:
            fe.lib.SimSetEraseBlock(erase_block)
        if cfi_block:
            fe.lib.SimSetCfiBlock(cfi_block)
        ctypes.memmove(fe.lib.SimRom(), rom if rom is not None else self.dump, fe.ROM_SIZE)

    def run(self, **kw):
        return fe.Run(self.image, syms=SW_SYMS, **kw).run()

    def checks(self, runs, expected=None):
        s = fe.stats()
        check(s.violations == 0, 'chip protocol violations: %d (%s)' % (s.violations, fe.lib.SimLastViolation()))
        check(s.badBankWrites == 0, 'bank register writes with wrong values: %d' % s.badBankWrites)
        block = fe.lib.SimEraseBlock()
        lo, hi = self.info['area']
        t = self.info['test']
        bad = [hex(e) for r in runs for e in r.erased
               if not (lo <= e and e + block <= hi) and not (t - 0x80000 <= e and e + block <= t + 0x80000)]
        check(not bad, 'erases outside the save area and the test area: %s' % bad[:5])
        if expected is not None:
            d = fe.first_diff(bytes(fe.rom_view()), expected)
            check(d is None, 'final ROM differs from the expected image at 0x%X' % (d or 0))


def main(argv):
    dump = open(argv[1], 'rb').read()
    dump = dump + b'\xff' * (fe.ROM_SIZE - len(dump))
    from emerald_rom import find_saves
    s = find_saves(dump[:0x1000000])[0]
    new_save = edited_save(dump[s:s + gen3save.SAVE_SIZE])
    code = open(os.path.join(fe.TOOLS, 'flasher', 'savewriter_code.bin'), 'rb').read()
    image, info = make_save_writer.build(dump, new_save, code)
    c = SaveCase(dump, image, info, new_save)
    print('save writer image %d bytes; save at 0x%X, area 0x%X-0x%X, test 0x%X; writing "%s"' % (
        len(image), info['save'], info['area'][0], info['area'][1], info['test'], gen3save.summary(new_save)))

    print('complete runs')
    for board, block in (('amd_swapped', 0x20000), ('amd_swapped', 0x10000), ('amd_dual', None),
                         ('intel_word', None), ('intel_buffered', 0x20000), ('intel_buffered', 0x40000)):
        c.setup(board, block)
        r = c.run()
        print('  %s%s: %d erases, %d program ops' % (board, ', %d KiB blocks' % (block >> 10) if block else '',
                                                    fe.stats().erases, fe.stats().programs))
        ok = check(r.finished == 0 and 'Done.' in r.screen(), 'did not finish:\n' + r.screen())
        c.checks([r], c.expected)
        if board == 'amd_swapped' and block == 0x20000 and ok:
            print('\n'.join('    | ' + l for l in r.screen().splitlines()))

    print('cart already flashed (v0.2: Colosseum program; v0.3: also the XD hook)')
    patch = open(os.path.join(fe.TOOLS, 'colosseum-mb-repro.bps'), 'rb').read()
    flasher_code = open(os.path.join(fe.TOOLS, 'flasher', 'flasher_code.bin'), 'rb').read()
    hook = open(os.path.join(fe.TOOLS, 'linkhook.bin'), 'rb').read()
    _, finfo = make_flasher.build(dump, patch, flasher_code, hook)
    for label, flashed in (('v0.2', finfo['colosseum']), ('v0.3', finfo['target'])):
        flashed = flashed + dump[len(flashed):]
        c.setup('amd_swapped', 0x20000, rom=flashed)
        r = c.run()
        check(r.finished == 0 and 'Done.' in r.screen(), '%s: did not finish on a flashed cart:\n%s' % (label, r.screen()))
        c.checks([r], flashed[:info['save']] + new_save + flashed[info['save'] + len(new_save):])
        # A save writer built from a dump made after flashing works the same.
        image2, _ = make_save_writer.build(flashed, new_save, code)
        check(image2 == image, '%s: a dump made after flashing gives a different save writer' % label)
        print('  %s: ok' % label)

    print('second run on a written cart')
    c.setup('amd_swapped', 0x20000)
    c.run()
    fe.lib.SimPowerCycle()
    before = (fe.stats().erases, fe.stats().programs)
    r = c.run()
    check(r.finished == 0 and 'already holds this' in r.screen(), 'second run:\n' + r.screen())
    check((fe.stats().erases, fe.stats().programs) == before, 'second run wrote to the cart')

    print('power cuts, then a second run')
    c.setup('amd_swapped', 0x20000)
    c.run()
    total = fe.Run.ops()
    points = sorted(set([1, 2, 3, 4, 5, 6, 8] + [total * k // 9 for k in range(1, 9)] + [total - 2, total - 1]))
    for n in points:
        c.setup('amd_swapped', 0x20000)
        r1 = c.run(stop_after_ops=n)
        fe.lib.SimPowerCycle()
        r2 = c.run()
        ok = check(r1.interrupted and r2.finished == 0 and 'Done.' in r2.screen(),
                   'cut at %d: second run did not finish\n%s' % (n, r2.screen()))
        c.checks([r1, r2], c.expected)
        print('  cut after %6d of %d ops: %s' % (n, total, 'ok' if ok else 'FAILED'))

    print('failing erases and programs')
    c.setup('amd_swapped', 0x20000)
    r = c.run(fail_program_at=[30000], fail_erase_at=[5])
    check(r.finished == 0 and 'Done.' in r.screen(), 'did not finish:\n' + r.screen())
    c.checks([r], c.expected)
    c.setup('intel_buffered', 0x40000)
    r = c.run(fail_program_at=[60, 61])
    check(r.finished == 0 and 'Done.' in r.screen() and r.errors, 'expected an error screen, then success:\n' + r.screen())
    c.checks([r], c.expected)

    print('refusals')
    other = bytearray(dump)
    other[0x200000] ^= 0x01
    for label, board, block, rom, message in (
            ('cart differs from the dump', 'amd_swapped', 0x20000, bytes(other), 'only writes to'),
            ('512 KiB blocks', 'intel_buffered', 0x80000, None, 'layout is not'),
            ('AMD x16 board', 'amd_x16', None, None, 'not supported')):
        c.setup(board, block, rom=rom)
        before = bytes(fe.rom_view())
        r = c.run()
        print('  ' + label)
        check(r.finished == 1 and message in r.screen().replace('\n', ' '), 'expected a refusal:\n' + r.screen())
        check(bytes(fe.rom_view()) == before and fe.stats().erases == 0 and fe.stats().programs == 0,
              'a refused run wrote to the cart')

    print('%d FAILURES' % fe.failures if fe.failures else 'all save writer tests passed')
    return 1 if fe.failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
