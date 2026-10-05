"""Reference decoder for the WLZ4 block format, written from doc/WLZ4_format.md alone (no code shared with src/).
It is slow and strict: it checks every rule of the specification and raises ValueError on any violation.

    python wlz4_decode.py block.wlz4 [out]          decode one block (optionally with --dict FILE)
    from wlz4_decode import decode; decode(block, dictionary=b'')
"""
import sys


def _ext(buf, pos):
    """An extension value (section 4.3): one byte below 252, else 251 + m followed by value - 252 in m bytes."""
    if pos >= len(buf):
        raise ValueError('truncated extension')
    b = buf[pos]
    pos += 1
    if b <= 251:
        return b, pos
    m = b - 251
    if pos + m > len(buf):
        raise ValueError('truncated extension')
    return 252 + int.from_bytes(buf[pos:pos + m], 'little'), pos + m


def decode(block, dictionary=b''):
    if len(block) < 2:
        raise ValueError('truncated size header')
    h = int.from_bytes(block[0:2], 'little')
    if h >> 15:                                          # 4-byte header
        if len(block) < 4:
            raise ValueError('truncated size header')
        size = (h & 0x7FFF) | int.from_bytes(block[2:4], 'little') << 15
        pos = 4
    else:
        size, pos = h, 2
    out = bytearray()
    hist = len(dictionary)
    while True:
        if pos >= len(block):
            raise ValueError('missing end marker')
        token = block[pos]
        pos += 1
        lit, code = token >> 4, token & 15
        if lit == 15:
            e, pos = _ext(block, pos)
            lit += e
        if pos + lit > len(block):
            raise ValueError('literal run past the input')
        out += block[pos:pos + lit]
        pos += lit
        # the offset field (section 4.4)
        if pos >= len(block):
            raise ValueError('missing offset')
        if code == 0:
            off, pos = block[pos], pos + 1
        else:
            nbytes = 1 + (code > 2) + (block[pos] & 1)
            if pos + nbytes > len(block):
                raise ValueError('truncated offset')
            off = int.from_bytes(block[pos:pos + nbytes], 'little') >> 1
            pos += nbytes
        if code == 15:
            e, pos = _ext(block, pos)
            mlen = 18 + e
        else:
            mlen = 3 + code
        if off == 0:                                     # the end marker (section 5)
            if code == 15:
                raise ValueError('zero offset after code 15')
            break
        if off > len(out) + hist:
            raise ValueError('offset before the history')
        start = len(out) - off
        if start >= 0 and off >= mlen:                   # no overlap: one slice
            out += out[start:start + mlen]
        else:                                            # byte by byte: the match overlaps its own output,
            for _ in range(mlen):                        # or starts in the dictionary
                j = len(out) - off
                out.append(out[j] if j >= 0 else dictionary[hist + j])
        if len(out) > size:
            raise ValueError('output exceeds the stored size')
    if pos != len(block):
        raise ValueError('bytes after the end marker')
    if len(out) != size:
        raise ValueError('decoded %d bytes, header says %d' % (len(out), size))
    return bytes(out)


if __name__ == '__main__':
    args = sys.argv[1:]
    d = b''
    if '--dict' in args:
        i = args.index('--dict')
        d = open(args[i + 1], 'rb').read()
        del args[i:i + 2]
    data = decode(open(args[0], 'rb').read(), d)
    if len(args) > 1:
        open(args[1], 'wb').write(data)
    else:
        print('%d bytes' % len(data))
