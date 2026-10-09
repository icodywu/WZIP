"""Reference decoder for WZ frames, written from doc/frame_format.md alone; the blocks go to the reference decoders
of the two codecs (wzip_decode.py, wlz4_decode.py). Slow and strict: it raises ValueError on any violation.

    python frame_decode.py file.wz [out]
    from frame_decode import decode; decode(data)
"""
import sys

import wlz4_decode
import wzip_decode

MAGIC = 0x0A5A578D
M = 0xFFFFFFFF
P1, P2, P3, P4, P5 = 0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D, 0x27D4EB2F, 0x165667B1


def _rotl(x, r):
    return ((x << r) | (x >> (32 - r))) & M


def xxh32(data, seed=0):
    """XXH32 as section 8 states it."""
    n, p = len(data), 0
    if n >= 16:
        v = [(seed + P1 + P2) & M, (seed + P2) & M, seed & M, (seed - P1) & M]
        while p + 16 <= n:
            for i in range(4):
                x = int.from_bytes(data[p + 4 * i:p + 4 * i + 4], 'little')
                v[i] = (_rotl((v[i] + x * P2) & M, 13) * P1) & M
            p += 16
        h = (_rotl(v[0], 1) + _rotl(v[1], 7) + _rotl(v[2], 12) + _rotl(v[3], 18)) & M
    else:
        h = (seed + P5) & M
    h = (h + n) & M
    while p + 4 <= n:
        h = (_rotl((h + int.from_bytes(data[p:p + 4], 'little') * P3) & M, 17) * P4) & M
        p += 4
    while p < n:
        h = (_rotl((h + data[p] * P5) & M, 11) * P1) & M
        p += 1
    h ^= h >> 15
    h = (h * P2) & M
    h ^= h >> 13
    h = (h * P3) & M
    h ^= h >> 16
    return h


def _u32(data, pos):
    if pos + 4 > len(data):
        raise ValueError('truncated frame')
    return int.from_bytes(data[pos:pos + 4], 'little')


def decode(data):
    """The content of a sequence of frames and skippable frames."""
    if not data:
        raise ValueError('no frame')
    out, pos = bytearray(), 0
    while pos < len(data):
        magic = _u32(data, pos)
        if 0x184D2A50 <= magic <= 0x184D2A5F:            # skippable frame (section 7)
            size = _u32(data, pos + 4)
            if pos + 8 + size > len(data):
                raise ValueError('truncated skippable frame')
            pos += 8 + size
            continue
        if magic != MAGIC:
            raise ValueError('not a WZ frame')
        if pos + 7 > len(data):
            raise ValueError('truncated frame header')
        flg, ver, bs = data[pos + 4], data[pos + 5], data[pos + 6]
        codec = flg & 3
        if codec > 1 or flg & 0xE0:
            raise ValueError('reserved codec or flag')
        if ver >> 4 != 0 or ver & 15 not in ((1, 2) if codec == 0 else (1,)):    # WZIP 2: sized blocks, filters
            raise ValueError('unsupported format version')
        if not 10 <= bs <= 31:
            raise ValueError('block size log out of range')
        pos += 7
        window = 0                                       # linked blocks (5.1): the window, 0 if independent
        if flg & 0x10:
            if codec != 0 or bs > 30:
                raise ValueError('linked blocks of WLZ4 or of 2^31 bytes')
            if pos >= len(data) or not 10 <= data[pos] <= 27:
                raise ValueError('window log missing or out of range')
            window = 1 << data[pos]
            pos += 1
        size = None
        if flg & 8:
            if pos + 8 > len(data):
                raise ValueError('truncated content size')
            size = int.from_bytes(data[pos:pos + 8], 'little')
            pos += 8
        content = bytearray()
        while True:
            v = _u32(data, pos)
            pos += 4
            if v == 0:
                break
            c, raw = v & 0x7FFFFFFF, v >> 31
            if c == 0 or pos + c > len(data):
                raise ValueError('empty or truncated block')
            block = bytes(data[pos:pos + c])
            pos += c
            if raw:
                if c > 1 << bs:
                    raise ValueError('raw block above the block size')
                content += block
                continue
            if codec == 1:
                d = wlz4_decode.decode(block)
            else:                                        # the dictionary: the last min(2^W, P) bytes of content
                d = wzip_decode.decode(block, bytes(content[len(content) - min(window, len(content)):]))
            if not 1 <= len(d) <= 1 << bs:
                raise ValueError('block decodes to a size out of range')
            content += d
        if flg & 4:
            if _u32(data, pos) != xxh32(content):
                raise ValueError('checksum mismatch')
            pos += 4
        if size is not None and size != len(content):
            raise ValueError('content size mismatch')
        out += content
    return bytes(out)


if __name__ == '__main__':
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)
    with open(args[0], 'rb') as f:
        result = decode(f.read())
    if len(args) > 1:
        with open(args[1], 'wb') as f:
            f.write(result)
    else:
        print(len(result), 'bytes, xxh32', hex(xxh32(result)))
