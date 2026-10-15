#!/usr/bin/env python3
"""Route B: replace the GBA link program on a Pokemon Colosseum disc image.

The GameCube sends a GBA program over the link cable when you transfer
Pokemon. Emerald normally throws that program away and runs its own copy;
it only does that when the program's header game code is "Gc6e". This tool
puts the repro build (header game code "TEST") on the disc, so Emerald runs
it as sent. No cart writer is needed.

usage:
  patch_colosseum_iso.py scan ISO
      List every GBA program found inside the disc's .fsys archives.
  patch_colosseum_iso.py patch ISO REPRO_BUILD OUTPUT_ISO
      Write a copy of ISO with the "Gc6e" program replaced by REPRO_BUILD
      (colosseum-mb-em-repro.gba). The original ISO is only read.
  patch_colosseum_iso.py extract ISO OUTPUT_FOLDER
      Copy every GBA program found anywhere on a GameCube disc (Colosseum,
      XD or another game) into OUTPUT_FOLDER as .gba files. The ISO is only
      read.

The replacement is written in place inside its .fsys archive, stored raw or
LZSS-compressed, whichever fits in the space the original occupied. If
neither fits, the tool stops and nothing is written.

Standard library only. Formats follow StarsMmd's GoD Tool (XGFsys/XGLZSS).
"""
import hashlib
import os
import shutil
import struct
import sys

COLOSSEUM_GAME_CODE = b'Gc6e'


# ------------------------------------------------------------------ LZSS
# Okumura LZSS as used by Colosseum/XD: 4 KiB window, 4-bit lengths,
# minimum match 3, window initialised to 0, write position starts at 0xFEE.

N = 4096
F = 18
THRESHOLD = 2


def lzss_decode(src, out_size=None):
    window = bytearray(N)
    r = N - F
    out = bytearray()
    i = 0
    flags = 0
    while i < len(src):
        flags >>= 1
        if not flags & 0x100:
            flags = src[i] | 0xFF00
            i += 1
            if i >= len(src):
                break
        if flags & 1:
            c = src[i]
            i += 1
            out.append(c)
            window[r] = c
            r = (r + 1) & (N - 1)
        else:
            if i + 1 >= len(src):
                break
            pos = src[i] | ((src[i + 1] & 0xF0) << 4)
            length = (src[i + 1] & 0x0F) + THRESHOLD
            i += 2
            for k in range(length + 1):
                c = window[(pos + k) & (N - 1)]
                out.append(c)
                window[r] = c
                r = (r + 1) & (N - 1)
        if out_size is not None and len(out) >= out_size:
            break
    return bytes(out[:out_size] if out_size is not None else out)


def lzss_encode(data):
    """Greedy LZSS with hash chains; output decodes with lzss_decode."""
    window = bytearray(N)
    r = N - F
    out = bytearray()
    chains = {}
    i = 0
    n = len(data)

    # The decoder's window starts zero-filled, so matches can also refer to
    # those zeros. Track window contents to find matches by content.
    def window_key(pos):
        return bytes(window[(pos + k) & (N - 1)] for k in range(3))

    # Seed chains with the initial zero window positions near r.
    for p in range(N - 2 * F, N - F):
        chains.setdefault(b'\0\0\0', []).append(p)

    while i < n:
        flag_pos = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if i >= n:
                break
            best_len, best_pos = 0, 0
            if i + 2 < n:
                key = bytes(data[i:i + 3])
                cands = chains.get(key, [])
                for p in reversed(cands[-64:]):
                    dist = (r - p) & (N - 1)
                    if dist == 0 or dist > N - F:
                        continue
                    length = 0
                    while length < F and i + length < n:
                        # byte the decoder will produce at this point
                        src_pos = (p + length) & (N - 1)
                        if ((src_pos - r) & (N - 1)) < length:
                            c = data[i + ((src_pos - r) & (N - 1))]
                        else:
                            c = window[src_pos]
                        if c != data[i + length]:
                            break
                        length += 1
                    if length > best_len:
                        best_len, best_pos = length, p
                        if length == F:
                            break
            if best_len > THRESHOLD:
                out.append(best_pos & 0xFF)
                out.append(((best_pos >> 4) & 0xF0) | (best_len - THRESHOLD - 1))
                take = best_len
            else:
                flags |= 1 << bit
                out.append(data[i])
                take = 1
            for k in range(take):
                c = data[i + k]
                window[r] = c
                if i + k + 2 < n:
                    chains.setdefault(bytes(data[i + k:i + k + 3]), []).append(r)
                r = (r + 1) & (N - 1)
            i += take
        out[flag_pos] = flags
    return bytes(out)


def lzss_file(data):
    body = lzss_encode(data)
    return b'LZSS' + struct.pack('>III', len(data), len(body) + 16, 0) + body


# ------------------------------------------------------------------ disc

class Iso:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        head = self.read(0, 0x440)
        if struct.unpack_from('>I', head, 0x1C)[0] != 0xC2339F3D:
            raise SystemExit('error: %s is not a GameCube disc image' % path)
        self.game_id = head[:6].decode('ascii', 'replace')
        fst_off, fst_size = struct.unpack_from('>II', head, 0x424)
        self.files = self._parse_fst(self.read(fst_off, fst_size))

    def read(self, offset, size):
        self.f.seek(offset)
        return self.f.read(size)

    @staticmethod
    def _parse_fst(fst):
        count = struct.unpack_from('>I', fst, 8)[0]
        strings = count * 12

        def name(i):
            off = strings + (struct.unpack_from('>I', fst, i * 12)[0] & 0xFFFFFF)
            return fst[off:fst.index(b'\0', off)].decode('ascii', 'replace')

        files = []
        dirs = [(count, '')]
        for i in range(1, count):
            while dirs and i >= dirs[-1][0]:
                dirs.pop()
            prefix = dirs[-1][1] if dirs else ''
            kind = fst[i * 12]
            a, b = struct.unpack_from('>II', fst, i * 12 + 4)
            if kind:
                dirs.append((b, prefix + name(i) + '/'))
            else:
                files.append((prefix + name(i), a, b))
        return files


class Fsys:
    def __init__(self, data):
        if data[:4] != b'FSYS':
            raise ValueError('not an FSYS archive')
        self.data = bytearray(data)
        self.count = struct.unpack_from('>I', data, 0x0C)[0]
        table = struct.unpack_from('>I', data, 0x40)[0]
        self.details = [struct.unpack_from('>I', data, table + 4 * i)[0] for i in range(self.count)]

    def start(self, i):
        return struct.unpack_from('>I', self.data, self.details[i] + 0x04)[0]

    def stored_size(self, i):
        return struct.unpack_from('>I', self.data, self.details[i] + 0x14)[0]

    def raw(self, i):
        s = self.start(i)
        return bytes(self.data[s:s + self.stored_size(i)])

    def extract(self, i):
        raw = self.raw(i)
        if raw[:4] == b'LZSS':
            size = struct.unpack_from('>I', raw, 4)[0]
            return lzss_decode(raw[16:], size)
        return raw

    def space(self, i):
        """Bytes available at entry i before the next entry's data."""
        s = self.start(i)
        later = [self.start(j) for j in range(self.count) if self.start(j) > s]
        end = min(later) if later else len(self.data) - 4
        return end - s

    def replace(self, i, new):
        s = self.start(i)
        old = self.stored_size(i)
        self.data[s:s + old] = bytes(old)
        self.data[s:s + len(new)] = new
        d = self.details[i]
        struct.pack_into('>I', self.data, d + 0x14, len(new))
        uncompressed = struct.unpack_from('>I', new, 4)[0] if new[:4] == b'LZSS' else len(new)
        struct.pack_into('>I', self.data, d + 0x08, uncompressed)


def is_gba_program(data):
    return len(data) >= 0xC0 and data[3] == 0xEA and data[0xB2] == 0x96


def find_programs(iso):
    """GBA programs inside .fsys archives, as (path, offset, size, index, fsys, data).
    Plain files that are GBA programs are listed with index None and fsys None."""
    found = []
    for path, off, size in iso.files:
        if not path.lower().endswith('.fsys'):
            if 0xC0 <= size <= 0x40000:
                data = iso.read(off, size)
                if is_gba_program(data):
                    found.append((path, off, size, None, None, data))
            continue
        try:
            fsys = Fsys(iso.read(off, size))
        except ValueError:
            continue
        for i in range(fsys.count):
            try:
                data = fsys.extract(i)
            except Exception:
                continue
            if is_gba_program(data):
                found.append((path, off, size, i, fsys, data))
    return found


LOGO_PREFIX = b'\x24\xff\xae\x51\x69\x9a\xa2\x21'


def program_offsets(data):
    """Offsets of GBA program headers in data: at 0, or wherever the logo appears."""
    found = [0] if is_gba_program(data) else []
    pos = data.find(LOGO_PREFIX, 5)
    while pos != -1:
        if is_gba_program(data[pos - 4:pos + 0xBC]):
            found.append(pos - 4)
        pos = data.find(LOGO_PREFIX, pos + 1)
    return found


def dol_size(head):
    ends = [struct.unpack_from('>I', head, i * 4)[0] + struct.unpack_from('>I', head, 0x90 + i * 4)[0]
            for i in range(18)]
    return max(ends)


def disc_containers(iso):
    """Every piece of data on the disc a GBA program could hide in, as (label, data):
    the boot DOL, each file, each .fsys entry (decompressed) and each LZSS file."""
    dol_off = struct.unpack_from('>I', iso.read(0x420, 4))[0]
    if dol_off:
        size = dol_size(iso.read(dol_off, 0x100))
        if size <= 0x4000000:
            yield 'main.dol', iso.read(dol_off, size)
    for path, off, size in iso.files:
        data = iso.read(off, size)
        if data[:4] == b'FSYS':
            try:
                fsys = Fsys(data)
            except Exception:
                fsys = None
            if fsys is not None:
                for i in range(fsys.count):
                    try:
                        yield '%s#%d' % (path, i), fsys.extract(i)
                    except Exception:
                        pass
        if data[:4] == b'LZSS':
            try:
                yield path + ' (LZSS)', lzss_decode(data[16:], struct.unpack_from('>I', data, 4)[0])
            except Exception:
                pass
        yield path, data


def cmd_extract(iso_path, out_dir):
    iso = Iso(iso_path)
    print('disc %s, %d files; searching for GBA programs...' % (iso.game_id, len(iso.files)))
    os.makedirs(out_dir, exist_ok=True)
    seen = set()
    count = 0
    for label, data in disc_containers(iso):
        for start in program_offsets(data):
            end = len(data) if start == 0 else min(len(data), start + 0x40000)
            program = data[start:end]
            digest = hashlib.sha1(program).hexdigest()
            if digest in seen:
                continue
            seen.add(digest)
            count += 1
            code = program[0xAC:0xB0].decode('ascii', 'replace')
            title = program[0xA0:0xAC].rstrip(b'\0').decode('ascii', 'replace')
            name = '%02d_%s_%s.gba' % (count, ''.join(c if c.isalnum() else '_' for c in code),
                                        ''.join(c if c.isalnum() else '_' for c in label)[-40:])
            with open(os.path.join(out_dir, name), 'wb') as f:
                f.write(program)
            print('  %s: in %s at 0x%X, %d bytes, game code %r, title %r, SHA-1 %s' % (
                name, label, start, len(program), code, title, digest))
    if not count:
        print('  no GBA programs found')
    return 0


def cmd_scan(iso_path):
    iso = Iso(iso_path)
    print('disc %s, %d files' % (iso.game_id, len(iso.files)))
    programs = find_programs(iso)
    for path, off, size, i, fsys, data in programs:
        code = data[0xAC:0xB0].decode('ascii', 'replace')
        if fsys is None:
            print('  %s: GBA program (plain file), game code %r, %d bytes' % (path, code, len(data)))
            continue
        print('  %s entry %d: GBA program, game code %r, %d bytes (%s), %d bytes of space' % (
            path, i, code, len(data), 'LZSS' if fsys.raw(i)[:4] == b'LZSS' else 'raw', fsys.space(i)))
    if not programs:
        print('  no GBA programs found')
    return 0


def cmd_patch(iso_path, image_path, out_path):
    image = open(image_path, 'rb').read()
    if not is_gba_program(image):
        raise SystemExit('error: %s is not a GBA program' % image_path)
    if image[0xAC:0xB0] == COLOSSEUM_GAME_CODE:
        raise SystemExit('error: the replacement has game code Gc6e, so Emerald would discard it')

    iso = Iso(iso_path)
    if not iso.game_id.startswith('GC6'):
        print('warning: disc ID %s is not Pokemon Colosseum' % iso.game_id)
    targets = [p for p in find_programs(iso) if p[5][0xAC:0xB0] == COLOSSEUM_GAME_CODE]
    if len(targets) != 1:
        raise SystemExit('error: expected one Gc6e program on the disc, found %d (run "scan")' % len(targets))
    path, off, size, i, fsys, old = targets[0]
    if fsys is None:
        raise SystemExit('error: the program is a plain file (%s); replace it with GoD Tool or a disc '
                         'rebuilder instead' % path)
    space = fsys.space(i)
    print('replacing %s entry %d (%d bytes, %d bytes of space)' % (path, i, len(old), space))

    # The image is zero-padded to 0x28000 bytes. The padding is not needed
    # (the program clears its own memory), so a trimmed copy may be sent.
    end = len(image)
    while end > 0xC0 and image[end - 1] == 0:
        end -= 1
    trimmed = image[:(end + 3) & ~3]

    new = None
    for label, data, compress in (('full, raw', image, False), ('trimmed, raw', trimmed, False),
                                  ('full, LZSS', image, True), ('trimmed, LZSS', trimmed, True)):
        candidate = lzss_file(data) if compress else data
        if compress and lzss_decode(candidate[16:], len(data)) != data:
            raise SystemExit('error: LZSS round trip failed')
        print('  %s: %d bytes' % (label, len(candidate)))
        if len(candidate) <= space:
            new, payload = candidate, data
            break
    if new is None:
        raise SystemExit('error: the program does not fit in the %d bytes the original used; '
                         'import it with GoD Tool (which can grow the archive) instead' % space)

    fsys.replace(i, new)
    check = Fsys(bytes(fsys.data))
    if check.extract(i) != payload:
        raise SystemExit('error: verification of the patched archive failed')

    print('copying disc image...')
    shutil.copyfile(iso_path, out_path)
    with open(out_path, 'r+b') as f:
        f.seek(off)
        f.write(fsys.data)
    print('wrote %s; %s entry %d now holds %s (%s, %d bytes stored)' % (
        out_path, path, i, os.path.basename(image_path), 'LZSS' if new[:4] == b'LZSS' else 'raw', len(new)))
    print('program sent to the GBA: %d bytes, SHA-1 %s' % (len(payload), hashlib.sha1(payload).hexdigest()))
    return 0


def main(argv):
    if len(argv) == 3 and argv[1] == 'scan':
        return cmd_scan(argv[2])
    if len(argv) == 5 and argv[1] == 'patch':
        return cmd_patch(argv[2], argv[3], argv[4])
    if len(argv) == 4 and argv[1] == 'extract':
        return cmd_extract(argv[2], argv[3])
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
