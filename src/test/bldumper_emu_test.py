#!/usr/bin/env python3
"""Runs the batteryless dumper's GBA program in the ARM emulator against the
simulated carts, with the Wii side of GBA Link Cable Dumper v1.6 replayed in
Python from its source/main.c: save backup, restore (confirmed, cancelled,
invalid, interrupted by a power cut), clear, game and BIOS dumps, no cart and
a cart without a batteryless save, on every supported board.

usage: bldumper_emu_test.py ROM_DUMP
"""
import ctypes
import os
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_INTR
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import flasher_emu_test as fe           # noqa: E402
import gen3save                         # noqa: E402

ELF = os.path.join(fe.TOOLS, 'flasher', 'build', 'bldumper.elf')
SYMS = fe.symbols(ELF)
IMAGE = open(os.path.join(fe.TOOLS, 'flasher', 'bldumper_code.bin'), 'rb').read()
check = fe.check
KEYS_CONFIRM = 0x301    # L+R+A
KEYS_CANCEL = 0x002     # B


# ------------------------------------------------------------------ Wii side
# Each generator yields ('recv',) to read JOY_TRANS (getting its value back)
# or ('send', value) to write JOY_RECV, in the order main.c does.

def wii_session(command, save=None, dump_words=0x100):
    if (yield ('recv',)) != 0:                       # "if(recv() == 0) //ready"
        return {'error': 'not ready'}
    gbasize = 0
    while gbasize == 0:
        gbasize = yield ('recv',)
    yield ('send', 0)
    savesize = yield ('recv',)
    yield ('send', 0)
    out = {'gbasize': gbasize, 'savesize': savesize}
    if gbasize == 0xFFFFFFFF:
        return out
    header = b''
    for _ in range(0x30):
        header += struct.pack('<I', (yield ('recv',)))
    out['header'] = header
    if command in (2, 3, 4) and savesize == 0:
        command = 0                                  # the Wii only offers Y/X/Z with a save
    yield ('send', command)
    if command == 1:                                 # dump: read only the start here
        data = b''
        for _ in range(dump_words):
            data += struct.pack('<I', (yield ('recv',)))
        out['dump'] = data
        out['stop'] = True                           # stop the run mid-dump
    elif command == 2:
        readval = 0
        while readval != savesize:
            readval = yield ('recv',)
        yield ('send', 0)
        data = b''
        for _ in range(savesize // 4):
            data += struct.pack('<I', (yield ('recv',)))
        out['save'] = data
    elif command in (3, 4):
        readval = 0
        while readval != savesize:
            readval = yield ('recv',)
        if command == 3:
            for i in range(0, savesize, 4):
                yield ('send', struct.unpack_from('<I', save, i)[0])
        while (yield ('recv',)) != 0:
            pass
        yield ('send', 0)
    return out


def wii_bios():
    yield ('send', 5)
    data = b''
    for _ in range(0x1000):
        data += struct.pack('<I', (yield ('recv',)))
    return {'bios': data}


class Run:
    """The GBA running the batteryless dumper, driven by a list of Wii scripts."""

    def __init__(self, scripts, keys=KEYS_CONFIRM, no_cart=False, stop_after_ops=None):
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(0x00000000, 0x4000)
        uc.mem_map(0x02000000, 0x40000)
        uc.mem_map(0x03000000, 0x8000)
        uc.mmio_map(0x04000000, 0x1000, self.io_read, None, self.io_write, None)
        uc.mem_map(0x05000000, 0x400)
        uc.mem_map(0x06000000, 0x18000)
        uc.mem_map(0x07000000, 0x400)
        uc.mmio_map(0x08000000, 0x02000000, self.rom_read, None, self.rom_write, None)
        uc.mmio_map(0x0E000000, 0x10000, self.sram_read, None, self.sram_write, None)
        uc.mem_write(0x02000000, IMAGE)
        for name, fn in (('WaitVBlank', self.ret0), ('ReadKeys', self.read_keys), ('ErrorWait', self.error_wait)):
            addr = SYMS[name] & ~1
            uc.hook_add(UC_HOOK_CODE, fn, begin=addr, end=addr)
        uc.hook_add(UC_HOOK_INTR, self.swi)
        self.uc = uc
        self.io = {}
        self.joycnt = 0
        self.trans = 0
        self.recv = 0
        self.keys = keys
        self.toggle = 0
        self.no_cart = no_cart
        self.stop_after_ops = stop_after_ops
        self.interrupted = False
        self.errors = []
        self.erased = []
        self.scripts = list(scripts)
        self.results = []
        self.gen = None
        self.polls = 0
        self.idle = 0
        self.next_script()

    def next_script(self):
        self.gen = self.scripts.pop(0)() if self.scripts else None
        self.op = next(self.gen) if self.gen else None

    def wii_step(self):
        """One SI transfer, every third time the GBA polls JOYCNT."""
        self.polls += 1
        if self.polls % 3:
            return
        if self.gen is None:
            self.idle += 1
            if self.idle > 50:              # let the GBA print, then stop
                self.uc.emu_stop()
            return
        value = None
        if self.op[0] == 'recv':
            value = self.trans
            self.joycnt |= 4
        else:
            self.recv = self.op[1]
            self.joycnt |= 2
        try:
            self.op = self.gen.send(value)
        except StopIteration as e:
            self.results.append(e.value)
            if e.value and e.value.get('stop'):
                self.uc.emu_stop()
                return
            self.next_script()

    # I/O
    def io_read(self, uc, offset, size, data):
        if offset == 0x140:
            self.wii_step()
            return self.joycnt
        if 0x150 <= offset < 0x154:
            return (self.recv >> ((offset - 0x150) * 8)) & ((1 << (size * 8)) - 1)
        if 0x154 <= offset < 0x158:
            return (self.trans >> ((offset - 0x154) * 8)) & ((1 << (size * 8)) - 1)
        if offset == 0x006:
            return 160
        return self.io.get(offset, 0)

    def io_write(self, uc, offset, size, value, data):
        if offset == 0x140:
            self.joycnt &= ~(value & 7)                 # write 1 to clear
        elif offset == 0x154:
            self.trans = value & (0xFFFFFFFF if size == 4 else 0xFFFF)
        else:
            self.io[offset] = value

    # cart bus
    def rom_read(self, uc, offset, size, data):
        if self.no_cart:                                 # open bus: each halfword its address
            lo = (offset >> 1) & 0xFFFF
            if size == 4:
                return lo | ((((offset >> 1) + 1) & 0xFFFF) << 16)
            return (lo >> ((offset & 1) * 8)) & 0xFF if size == 1 else lo
        if size == 1:
            return (fe.lib.HostRomRead16(offset & ~1) >> ((offset & 1) * 8)) & 0xFF
        if size == 2:
            return fe.lib.HostRomRead16(offset)
        return fe.lib.HostRomRead16(offset) | (fe.lib.HostRomRead16(offset + 2) << 16)

    def rom_write(self, uc, offset, size, value, data):
        if offset == 0x01000000 and size == 1:
            fe.lib.HostBankWrite8(value)
            return
        assert size == 2, 'unexpected %d-byte cart write' % size
        before = fe.stats().erases
        fe.lib.HostRomWrite16(offset, value)
        if fe.stats().erases != before:
            self.erased.append(fe.lib.SimLastErase())
        if self.stop_after_ops is not None and fe.Run.ops() >= self.stop_after_ops and not self.interrupted:
            self.interrupted = True
            uc.emu_stop()

    def sram_read(self, uc, offset, size, data):
        return fe.lib.HostSramRead8(offset)

    def sram_write(self, uc, offset, size, value, data):
        fe.lib.HostSramWrite8(offset, value)

    # hooked functions and BIOS calls
    def ret(self, uc, value):
        uc.reg_write(UC_ARM_REG_R0, value)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def ret0(self, uc, address, size, data):
        self.ret(uc, 0)

    def read_keys(self, uc, address, size, data):
        self.toggle ^= 1
        self.ret(uc, self.keys if self.toggle == 0 else 0)

    def error_wait(self, uc, address, size, data):
        self.errors.append(uc.reg_read(UC_ARM_REG_R0))
        self.ret(uc, 0)

    def swi(self, uc, intno, data):
        thumb = uc.reg_read(UC_ARM_REG_CPSR) & 0x20
        pc = uc.reg_read(UC_ARM_REG_PC)
        num = uc.mem_read(pc - 2, 1)[0] if thumb else uc.mem_read(pc - 4 + 2, 1)[0]
        if num != 0x1F:
            raise RuntimeError('unexpected SWI 0x%X' % num)
        uc.reg_write(UC_ARM_REG_R0, (uc.reg_read(UC_ARM_REG_R0) * 0x9E3779B1) & 0xFFFFFFFF)

    def run(self):
        self.uc.emu_start(0x020000C0, 0xFFFFFFFF, timeout=900 * 1000000)
        return self

    def screen(self):
        vram = self.uc.mem_read(0x0600F800, 32 * 20 * 2)
        lines = []
        for y in range(20):
            row = struct.unpack_from('<30H', vram, y * 64)
            lines.append(''.join(chr((e & 0x3FF) + 0x20) if (e & 0x3FF) < 0x5F else '#' for e in row).rstrip())
        return '\n'.join(lines).rstrip()


def setup(board, rom, erase_block=None):
    fe.lib.SimInit(fe.BOARDS[board], 5)
    if erase_block:
        fe.lib.SimSetEraseBlock(erase_block)
    ctypes.memmove(fe.lib.SimRom(), rom, fe.ROM_SIZE)


def edited_save(save):
    save = bytearray(save)
    _, _, sb2 = gen3save.newest(save)
    save[sb2:sb2 + 7] = bytes([0xCE, 0xBF, 0xCD, 0xCE, 0xBF, 0xCC, 0xFF])      # "TESTER"
    struct.pack_into('<H', save, sb2 + 0xFF6, gen3save.checksum(save[sb2:sb2 + gen3save.EMERALD_SIZES[0]]))
    return bytes(save)


def main(argv):
    dump = open(argv[1], 'rb').read()
    dump = dump + b'\xff' * (fe.ROM_SIZE - len(dump))
    from emerald_rom import find_saves
    s = find_saves(dump[:0x1000000])[0]
    old = dump[s:s + 0x20000]
    new = edited_save(old)
    expected = dump[:s] + new + dump[s + 0x20000:]
    print('batteryless dumper program %d bytes; save at 0x%X' % (len(IMAGE), s))

    def flash_checks(runs, allowed_extra=()):
        st = fe.stats()
        check(st.violations == 0, 'chip protocol violations: %d (%s)' % (st.violations, fe.lib.SimLastViolation()))
        check(st.badBankWrites == 0, 'bank register writes with wrong values: %d' % st.badBankWrites)
        block = fe.lib.SimEraseBlock()
        bad = [hex(e) for r in runs for e in r.erased
               if not (0xFC0000 <= e and e + block <= 0x1000000) and not (0xE80000 <= e and e + block <= 0xF80000)]
        check(not bad, 'erases outside the save area and the test area: %s' % bad[:5])

    print('backup')
    setup('amd_swapped', dump, 0x20000)
    r = Run([lambda: wii_session(2)]).run()
    res = r.results[0] if r.results else {}
    check(res.get('gbasize') == 0x2000000 and res.get('savesize') == 0x20000, 'sizes: %s' % {k: res.get(k) for k in ('gbasize', 'savesize')})
    check(res.get('header') == dump[:0xC0], 'header differs')
    check(res.get('save') == old, 'backed-up save differs from the save area')
    check(fe.stats().erases == 0 and fe.stats().programs == 0, 'a backup wrote to the cart')
    print('\n'.join('    | ' + l for l in r.screen().splitlines()))

    print('restore, then backup again in the same session')
    for board, block in (('amd_swapped', 0x20000), ('amd_swapped', 0x10000), ('amd_dual', None),
                         ('intel_word', None), ('intel_buffered', 0x20000), ('intel_buffered', 0x40000)):
        setup(board, dump, block)
        r = Run([lambda: wii_session(3, new), lambda: wii_session(2)]).run()
        ok = check(len(r.results) == 2 and 'Save written.' in r.screen(), '%s: restore did not finish:\n%s' % (board, r.screen()))
        check(bytes(fe.rom_view()) == expected, '%s: ROM after restore differs at 0x%X' % (board, fe.first_diff(bytes(fe.rom_view()), expected) or 0))
        check(ok and r.results[1].get('save') == new, '%s: backup after restore differs' % board)
        flash_checks([r])
        print('  %s%s: %d erases, %d program ops' % (board, ', %d KiB blocks' % (block >> 10) if block else '',
                                                    fe.stats().erases, fe.stats().programs))
        if board == 'amd_swapped' and block == 0x20000:
            print('\n'.join('    | ' + l for l in r.screen().splitlines()))

    print('restore cancelled with B')
    setup('amd_swapped', dump, 0x20000)
    r = Run([lambda: wii_session(3, new)], keys=KEYS_CANCEL).run()
    check('Cancelled' in r.screen() and r.results, 'cancel:\n' + r.screen())
    check(bytes(fe.rom_view()) == dump and fe.stats().erases == 0, 'a cancelled restore wrote to the cart')

    print('restore of a file that is not a save')
    setup('amd_swapped', dump, 0x20000)
    junk = bytes((i * 37 + 11) & 0xFF for i in range(0x20000))
    r = Run([lambda: wii_session(3, junk)]).run()
    check('not a valid' in r.screen() and r.results, 'junk restore:\n' + r.screen())
    check(bytes(fe.rom_view()) == dump and fe.stats().erases == 0, 'a junk restore wrote to the cart')
    broken = bytearray(new)
    broken[0x100] ^= 0xFF                              # breaks a checksum in one slot only
    r = Run([lambda: wii_session(3, bytes(broken))]).run()
    check('Save written.' in r.screen(), 'a save with one damaged slot should still be written:\n' + r.screen())

    print('restore interrupted by power cuts, then restored again')
    setup('amd_swapped', dump, 0x20000)
    Run([lambda: wii_session(3, new)]).run()
    total = fe.Run.ops()
    for n in sorted(set([1, 3, 5, 7] + [total * k // 6 for k in range(1, 6)] + [total - 1])):
        setup('amd_swapped', dump, 0x20000)
        r1 = Run([lambda: wii_session(3, new)], stop_after_ops=n).run()
        fe.lib.SimPowerCycle()
        r2 = Run([lambda: wii_session(3, new)]).run()
        ok = check(r1.interrupted and 'Save written.' in r2.screen() or 'already holds' in r2.screen(),
                   'cut at %d: second restore did not finish:\n%s' % (n, r2.screen()))
        ok &= check(bytes(fe.rom_view()) == expected, 'cut at %d: ROM differs' % n)
        flash_checks([r1, r2])
        print('  cut after %5d of %d ops: %s' % (n, total, 'ok' if ok else 'FAILED'))

    print('clear, game dump, BIOS dump')
    setup('amd_swapped', dump, 0x20000)
    r = Run([lambda: wii_session(4), wii_bios, lambda: wii_session(1, dump_words=0x4000)]).run()
    check(len(r.results) == 3, 'not all three finished: %s' % [list(x) for x in r.results])
    check('Nothing was changed' in r.screen() and bytes(fe.rom_view()) == dump, 'clear changed something')
    check(len(r.results) > 1 and len(r.results[1].get('bios', b'')) == 0x4000, 'BIOS dump size')
    check(len(r.results) > 2 and r.results[2].get('dump') == dump[:0x10000], 'game dump start differs')

    print('no cart; empty save area; cart without batteryless code')
    setup('amd_swapped', dump, 0x20000)
    r = Run([lambda: wii_session(2)], no_cart=True).run()
    check(r.results and r.results[0].get('gbasize') == 0xFFFFFFFF and 'No cart' in r.screen(), 'no cart:\n' + r.screen())
    empty = dump[:s] + b'\xff' * 0x20000 + dump[s + 0x20000:]
    setup('amd_swapped', empty, 0x20000)
    r = Run([lambda: wii_session(3, new)]).run()
    check(r.results and r.results[0].get('savesize') == 0x20000 and 'holds no valid save' in r.screen()
          and 'Save written.' in r.screen(), 'empty save area (found through the boot code):\n' + r.screen())
    check(bytes(fe.rom_view()) == expected, 'restore into an empty save area: ROM differs')
    plain = struct.pack('<I', 0xEA00002E) + empty[4:]          # entry point as on an unpatched cart
    setup('amd_swapped', plain, 0x20000)
    r = Run([lambda: wii_session(2)]).run()
    check(r.results and r.results[0].get('savesize') == 0 and 'No batteryless save' in r.screen(), 'no save:\n' + r.screen())
    setup('amd_swapped', struct.pack('<I', 0xEA00002E) + dump[4:], 0x20000)   # other batteryless code
    r = Run([lambda: wii_session(2)]).run()
    check(r.results and r.results[0].get('save') == old, 'save found by its contents without the boot code')

    print('%d FAILURES' % fe.failures if fe.failures else 'all batteryless dumper tests passed')
    return 1 if fe.failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
