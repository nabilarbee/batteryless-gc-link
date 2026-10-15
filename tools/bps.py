#!/usr/bin/env python3
"""Minimal BPS patch format (byuu/beat) creator and applier.

usage:
  bps.py create SOURCE TARGET PATCH
  bps.py apply SOURCE PATCH TARGET
"""
import struct
import sys
import zlib

SOURCE_READ, TARGET_READ, SOURCE_COPY, TARGET_COPY = range(4)


def _encode_number(n):
    out = bytearray()
    while True:
        x = n & 0x7F
        n >>= 7
        if n == 0:
            out.append(0x80 | x)
            return bytes(out)
        out.append(x)
        n -= 1


def _decode_number(data, pos):
    n, shift = 0, 1
    while True:
        x = data[pos]
        pos += 1
        n += (x & 0x7F) * shift
        if x & 0x80:
            return n, pos
        shift <<= 7
        n += shift


def create(source, target, metadata=b''):
    out = bytearray(b'BPS1')
    out += _encode_number(len(source)) + _encode_number(len(target)) + _encode_number(len(metadata)) + metadata
    i = 0
    n = len(target)
    while i < n:
        # Run of bytes that match the source at the same offset
        j = i
        while j < n and j < len(source) and source[j] == target[j]:
            j += 1
        if j - i >= 4 or (j == n and j > i):
            out += _encode_number(((j - i - 1) << 2) | SOURCE_READ)
            i = j
            continue
        # Literal run until the next same-offset match of at least 4 bytes
        j = i
        while j < n:
            k = j
            while k < n and k < len(source) and source[k] == target[k] and k - j < 4:
                k += 1
            if k - j >= 4:
                break
            j += 1
        out += _encode_number(((j - i - 1) << 2) | TARGET_READ) + target[i:j]
        i = j
    out += struct.pack('<II', zlib.crc32(source), zlib.crc32(target))
    out += struct.pack('<I', zlib.crc32(out))
    return bytes(out)


def apply(source, patch):
    if patch[:4] != b'BPS1':
        raise ValueError('not a BPS patch')
    if zlib.crc32(patch[:-4]) != struct.unpack_from('<I', patch, len(patch) - 4)[0]:
        raise ValueError('patch file is corrupt')
    src_crc, tgt_crc = struct.unpack_from('<II', patch, len(patch) - 12)
    if zlib.crc32(source) != src_crc:
        raise ValueError('source does not match the patch (wrong input file)')
    pos = 4
    src_size, pos = _decode_number(patch, pos)
    tgt_size, pos = _decode_number(patch, pos)
    meta_size, pos = _decode_number(patch, pos)
    pos += meta_size
    if src_size != len(source):
        raise ValueError('source size mismatch')
    target = bytearray()
    src_rel = tgt_rel = 0
    end = len(patch) - 12
    while pos < end:
        data, pos = _decode_number(patch, pos)
        cmd, length = data & 3, (data >> 2) + 1
        if cmd == SOURCE_READ:
            target += source[len(target):len(target) + length]
        elif cmd == TARGET_READ:
            target += patch[pos:pos + length]
            pos += length
        else:
            off, pos = _decode_number(patch, pos)
            off = (-1 if off & 1 else 1) * (off >> 1)
            if cmd == SOURCE_COPY:
                src_rel += off
                target += source[src_rel:src_rel + length]
                src_rel += length
            else:
                tgt_rel += off
                for _ in range(length):
                    target.append(target[tgt_rel])
                    tgt_rel += 1
    if len(target) != tgt_size or zlib.crc32(target) != tgt_crc:
        raise ValueError('patched output failed its checksum')
    return bytes(target)


def main(argv):
    if len(argv) == 5 and argv[1] == 'create':
        patch = create(open(argv[2], 'rb').read(), open(argv[3], 'rb').read())
        open(argv[4], 'wb').write(patch)
        print('wrote %s (%d bytes)' % (argv[4], len(patch)))
        return 0
    if len(argv) == 5 and argv[1] == 'apply':
        open(argv[4], 'wb').write(apply(open(argv[2], 'rb').read(), open(argv[3], 'rb').read()))
        print('wrote', argv[4])
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
