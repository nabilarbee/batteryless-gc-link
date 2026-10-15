#!/usr/bin/env python3
"""Makes the batteryless edition of FIX94's GBA Link Cable Dumper.

Puts a new GBA side (bldumper_code.bin) into the dumper's boot.dol and keeps
the Wii side as it is. With a batteryless repro cart inserted, the dumper's
"backup save" (Y) and "restore save" (X) then read and write the save the
cart keeps in its ROM: about 12 seconds instead of a 50 minute ROM dump.
Restores are shown and confirmed on the GBA screen. Game and BIOS dumps work
as before; for saves on normal carts, keep using the original dumper.

usage:
  make_batteryless_dumper.py BOOT.DOL OUTPUT_FOLDER

BOOT.DOL is GBA Link Cable Dumper v1.6 for the Wii (its boot.dol from the
apps folder of your SD card). OUTPUT_FOLDER receives boot.dol and meta.xml:
copy it into SD:/apps/ as its own folder, next to the original dumper.
Standard library only; BOOT.DOL itself is never modified.
"""
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CODE_PATH = os.path.join(HERE, 'bldumper_code.bin')

# The GBA side of v1.6 prints this at start-up; the Wii side prints a different line.
MARKER = b'\x1b[9;2HGBA Link Cable Dumper v1.6'
MAX_PAYLOAD = 0x40000
RECEIVE_BUFFER = 0x20000      # bldumper keeps a restored save at 0x02020000
META_XML = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<app version="1">
  <name>GBA Link Cable Dumper (batteryless)</name>
  <coder>FIX94, batteryless GBA side added</coder>
  <version>1.6-bl0.1</version>
  <short_description>Saves of batteryless repro carts</short_description>
  <long_description>FIX94's GBA Link Cable Dumper v1.6 with a new GBA side: backup save and restore save read and write the save that batteryless repro carts keep in ROM. Restores are confirmed on the GBA. For saves on normal carts, use the original dumper.</long_description>
</app>
'''


def be32(data, pos):
    return struct.unpack_from('>I', data, pos)[0]


def dol_sections(dol):
    """(file offset, size) of every text and data section."""
    out = []
    for i in range(18):
        off, size = be32(dol, i * 4), be32(dol, 0x90 + i * 4)
        if size:
            out.append((off, size))
    return out


def header_ok(data, start):
    """A multiboot image header: ARM branches at 0 and 0xC0, fixed byte 0x96,
    valid complement check."""
    if start < 0 or start + 0xC4 > len(data):
        return False
    if data[start + 3] != 0xEA or data[start + 0xC3] != 0xEA or data[start + 0xB2] != 0x96:
        return False
    chk = 0
    for b in data[start + 0xA0:start + 0xBD]:
        chk = (chk - b) & 0xFF
    return data[start + 0xBD] == (chk - 0x19) & 0xFF


def find_payload(dol):
    """(start, size, offset of the size field) of the embedded GBA program."""
    hits = []
    pos = dol.find(MARKER)
    while pos >= 0:
        hits.append(pos)
        pos = dol.find(MARKER, pos + 1)
    if len(hits) != 1:
        raise SystemExit('error: this is not GBA Link Cable Dumper v1.6 (or it is already patched)')
    marker = hits[0]
    starts = [s for s in range(marker - 0xC4, max(-1, marker - MAX_PAYLOAD), -1) if header_ok(dol, s)]
    if not starts:
        raise SystemExit('error: could not find the start of the GBA program')
    start = starts[0]
    # bin2o puts the size, a big-endian word, right after the data (plus alignment).
    fields = [q for q in range(marker, min(len(dol) - 3, start + MAX_PAYLOAD + 32))
              if start + be32(dol, q) <= q < start + be32(dol, q) + 32 and be32(dol, q) > marker - start]
    if len(fields) != 1:
        raise SystemExit('error: could not find the size of the GBA program (%d candidates)' % len(fields))
    size_at = fields[0]
    size = be32(dol, size_at)
    if not any(off <= start and size_at + 4 <= off + sz for off, sz in dol_sections(dol)):
        raise SystemExit('error: the GBA program is not inside one section of the dol')
    return start, size, size_at


def build(dol, code):
    start, size, size_at = find_payload(dol)
    payload = bytearray(code)
    payload += b'\0' * (-len(payload) % 4)
    payload[0x04:0xA0] = dol[start + 0x04:start + 0xA0]     # Nintendo logo, from the original
    payload[0xA0:0xB0] = b'BATTERYLESSDBLDP'                  # title and game code
    chk = 0
    for b in payload[0xA0:0xBD]:
        chk = (chk - b) & 0xFF
    payload[0xBD] = (chk - 0x19) & 0xFF
    if len(payload) > size:
        raise SystemExit('error: the new GBA program (%d bytes) does not fit in place of the old one (%d bytes)'
                         % (len(payload), size))
    if len(payload) > RECEIVE_BUFFER:
        raise SystemExit('error: the new GBA program is too large')
    out = bytearray(dol)
    out[start:start + size] = bytes(payload) + b'\0' * (size - len(payload))
    struct.pack_into('>I', out, size_at, len(payload))
    # Check the result the same way an unpatched file is read.
    assert header_ok(out, start) and be32(out, size_at) == len(payload)
    assert out[:start] == dol[:start] and out[start + size:size_at] == dol[start + size:size_at]
    assert out[size_at + 4:] == dol[size_at + 4:]
    return bytes(out), {'start': start, 'old_size': size, 'new_size': len(payload)}


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    dol_path, out_dir = argv[1:]
    dol = open(dol_path, 'rb').read()
    out, info = build(dol, open(CODE_PATH, 'rb').read())
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, 'boot.dol')
    if os.path.exists(out_path) and os.path.samefile(out_path, dol_path):
        raise SystemExit('error: choose an output folder other than the original dumper\'s')
    open(out_path, 'wb').write(out)
    open(os.path.join(out_dir, 'meta.xml'), 'w', newline='\n').write(META_XML)
    print('GBA program found at 0x%X in the dol (%d bytes); replaced with %d bytes'
          % (info['start'], info['old_size'], info['new_size']))
    print('wrote %s (SHA-1 %s) and meta.xml' % (out_path, hashlib.sha1(out).hexdigest()))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
