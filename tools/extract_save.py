#!/usr/bin/env python3
"""Copies the save out of a dump of your batteryless repro Emerald cart into
a 128 KiB .sav file that PKHeX (or an emulator) can open.

usage:
  extract_save.py ROM_DUMP OUTPUT.sav

The dump holds the save as it was when the dump was made. Standard library
only; ROM_DUMP is never modified.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen3save                         # noqa: E402
from emerald_rom import find_saves      # noqa: E402


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    rom_path, out_path = argv[1:]
    if os.path.abspath(rom_path) == os.path.abspath(out_path):
        raise SystemExit('error: write the save to a new file')
    rom = open(rom_path, 'rb').read()
    saves = find_saves(rom[:0x1000000])
    if len(saves) != 1:
        raise SystemExit('error: expected one save area in the dump, found %d' % len(saves))
    save = rom[saves[0]:saves[0] + gen3save.SAVE_SIZE]
    for slot in range(2):
        counter, _, problems = gen3save.check_slot(save, slot)
        print('slot %s: %s' % ('AB'[slot], 'save count %d, ok' % counter if not problems else '; '.join(problems[:3])))
    if gen3save.newest(save) is None:
        raise SystemExit('error: no valid save in the dump')
    open(out_path, 'wb').write(save)
    print('save area at 0x%06X: %s' % (saves[0], gen3save.summary(save)))
    print('wrote %s (%d bytes)' % (out_path, len(save)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
