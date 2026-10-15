#!/usr/bin/env python3
"""Checks make_batteryless_dumper.py on a synthetic boot.dol laid out like
devkitPPC's bin2o output (a 32-byte aligned GBA program inside a data section,
followed by its size as a big-endian word), and on a real boot.dol if one is
given.

usage: patcher_test.py ROM_DUMP [BOOT.DOL]
"""
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..')))
import make_batteryless_dumper as mbd     # noqa: E402

failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print('  FAIL:', msg)


def fake_payload(logo, size, rnd):
    p = bytearray(rnd.getrandbits(8) for _ in range(size))
    struct.pack_into('<I', p, 0, 0xEA00002E)
    p[4:0xA0] = logo
    p[0xA0:0xB0] = b'GBALINKDUMP\0\0\0\0\0'
    p[0xB2] = 0x96
    chk = 0
    for b in p[0xA0:0xBD]:
        chk = (chk - b) & 0xFF
    p[0xBD] = (chk - 0x19) & 0xFF
    struct.pack_into('<I', p, 0xC0, 0xEA000006)
    p[0x3000:0x3000 + len(mbd.MARKER) + 1] = mbd.MARKER + b'\n'
    return bytes(p)


def fake_dol(payload, rnd, pad_size_field=True):
    text = bytes(rnd.getrandbits(8) for _ in range(0x8000))
    before = bytes(rnd.getrandbits(8) for _ in range(0x1220))
    data = bytearray(before)
    data += b'\0' * (-len(data) % 32)
    start_in_data = len(data)
    data += payload
    if pad_size_field:
        data += b'\0' * (-len(data) % 4)
    data += struct.pack('>I', len(payload))
    data += bytes(rnd.getrandbits(8) for _ in range(0x2000))
    hdr = bytearray(0x100)
    struct.pack_into('>I', hdr, 0x00, 0x100)                 # text 0 offset
    struct.pack_into('>I', hdr, 0x1C, 0x100 + len(text))     # data 0 offset
    struct.pack_into('>I', hdr, 0x48, 0x80003100)
    struct.pack_into('>I', hdr, 0x64, 0x80010000)
    struct.pack_into('>I', hdr, 0x90, len(text))
    struct.pack_into('>I', hdr, 0xAC, len(data))
    struct.pack_into('>I', hdr, 0xE0, 0x80003100)
    return bytes(hdr + text + data), 0x100 + len(text) + start_in_data


def main(argv):
    rom = open(argv[1], 'rb').read(0xC0)
    logo = rom[4:0xA0]
    code = open(mbd.CODE_PATH, 'rb').read()
    rnd = random.Random(1)
    for size, padded in ((40000, True), (len(code) + 3, False), (0x9FFF, True)):
        payload = fake_payload(logo, size, rnd)
        dol, start = fake_dol(payload, rnd, padded)
        try:
            out, info = mbd.build(dol, code)
        except SystemExit as e:
            check(size < len(code) + (-len(code) % 4), 'size %d: refused: %s' % (size, e))
            continue
        new = len(code) + (-len(code) % 4)
        check(info['start'] == start and info['old_size'] == size and info['new_size'] == new, 'size %d: found %s' % (size, info))
        check(out[start:start + new][0xC0:] == (code + b'\0' * 4)[0xC0:new], 'size %d: code not in place' % size)
        check(out[start + 4:start + 0xA0] == logo, 'size %d: logo not kept' % size)
        check(mbd.header_ok(out, start), 'size %d: header check fails' % size)
        check(out[start + new:start + size] == b'\0' * (size - new), 'size %d: old bytes left behind' % size)
        diff = [i for i in range(len(dol)) if dol[i] != out[i] and not start <= i < start + size]
        check(len(diff) <= 4 and all(dol[i] != out[i] for i in diff), 'size %d: bytes changed outside: %s' % (size, diff[:8]))
        try:
            mbd.build(out, code)
            check(False, 'size %d: patching twice should be refused' % size)
        except SystemExit:
            pass
        print('  synthetic dol, original program %d bytes: ok' % size)
    small = fake_payload(logo, 0x2000, rnd)
    dol, _ = fake_dol(small, rnd)
    try:
        mbd.build(dol, code)
        check(False, 'a program too large for the slot should be refused')
    except SystemExit as e:
        print('  too large: refused (%s)' % e)
    if len(argv) > 2:
        dol = open(argv[2], 'rb').read()
        out, info = mbd.build(dol, code)
        print('  real boot.dol: GBA program at 0x%X, %d bytes, replaced with %d bytes' % (
            info['start'], info['old_size'], info['new_size']))
    print('%d FAILURES' % failures if failures else 'all patcher tests passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
