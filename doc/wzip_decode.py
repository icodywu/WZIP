"""Reference decoder for the WZIP formats (doc/WZIP_format.md): the one-call stream of wzip_compress (WZIP_L and
WZIP_M) and WZIP_S blocks. Written from the specification; it shares no code with src/, is slow, and checks the
rules of the specification, raising ValueError on any violation.

    python wzip_decode.py stream.wzip [out]              a wzip_compress stream
    python wzip_decode.py --s block.wzips [out]          a WZIP_S block (add --dict FILE for a dictionary)
    from wzip_decode import decode, decode_s
"""
import sys

# ------------------------------------------------------------------------------------------------ bit reading (2)

class Bits:
    """An MSB-first bit reader: bit 7 of byte 0 comes first. Reading past the end yields zero bits (decoders
    must not depend on them; the callers check that each stream ends where it should)."""

    def __init__(self, data, pos=0):
        self.d, self.p = data, pos * 8            # p: position in bits

    def read(self, n):
        if n == 0:
            return 0
        byte, off = self.p >> 3, self.p & 7
        nbytes = (off + n + 7) >> 3
        chunk = self.d[byte:byte + nbytes]
        if len(chunk) < nbytes:
            chunk = bytes(chunk) + bytes(nbytes - len(chunk))
        self.p += n
        return (int.from_bytes(chunk, 'big') >> (8 * nbytes - off - n)) & ((1 << n) - 1)

    def peek(self, n):
        p = self.p
        v = self.read(n)
        self.p = p
        return v

    def align(self):
        """skips to the next byte boundary; returns the byte position"""
        self.p = (self.p + 7) & ~7
        return self.p >> 3

    def byte_pos(self):
        return (self.p + 7) >> 3

# ------------------------------------------------------------------------------------------- Huffman codes (3)

class Code:
    """A canonical prefix code from code lengths: codes are assigned in order of increasing length, and among equal
    lengths in increasing symbol order, starting from 0 (as DEFLATE assigns them)."""

    def __init__(self, lengths):
        self.max = max(lengths) if lengths else 0
        self.table = None
        if self.max == 0:
            return
        order = sorted((L, s) for s, L in enumerate(lengths) if L)
        self.table = [None] * (1 << self.max)
        code, prev = 0, order[0][0]
        for L, s in order:
            code <<= L - prev
            prev = L
            first = code << (self.max - L)
            for i in range(first, first + (1 << (self.max - L))):
                self.table[i] = (s, L)
            code += 1

    def decode(self, bits):
        if self.table is None:
            raise ValueError('symbol read from an empty code')
        e = self.table[bits.peek(self.max)]
        if e is None:
            raise ValueError('bits that no code word starts')      # only a one-symbol code leaves gaps
        bits.p += e[1]
        return e[0]


def check_code(lengths, cap):
    """The longest length, 0 for an empty code; a code must be complete (Kraft sum 1) or one symbol of length 1."""
    used = [L for L in lengths if L]
    if any(L > cap for L in lengths):
        raise ValueError('code length above the cap')
    if not used:
        return 0
    if len(used) == 1:
        if used[0] != 1:
            raise ValueError('a one-symbol code must have length 1')
        return 1
    if sum(1 << (cap - L) for L in used) != 1 << cap:
        raise ValueError('incomplete or oversubscribed code')
    return max(used)


def read_lengths_plain(bits, n, cap):
    """(3.2) the lengths of the weight code: N_Bits(cap) bits each; after a length equal to the previous one, a
    repeat count r (2 bits; 3 -> 3 + 4 bits; 18 -> 18 + 8 bits) of further copies"""
    w = cap.bit_length()
    out = [bits.read(w)]
    while len(out) < n:
        L = bits.read(w)
        out.append(L)
        if L == out[-2]:
            r = bits.read(2)
            if r == 3:
                r += bits.read(4)
                if r == 18:
                    r += bits.read(8)
            if len(out) + r > n:
                raise ValueError('repeat past the end of the table')
            out += [L] * r
    return out


REP_LEN, REP_ZERO = 13, 14


def read_lengths_by_code(bits, wcode, n):
    """(3.3) a table's lengths coded by the weight code: symbols 0-12 are lengths; 13 repeats the previous length
    2 + r times (r: 2 bits; 5 -> 5 + 4 bits; 20 -> 20 + 8 bits); 14 gives 2 + r zeros (r: 3 bits; 9 -> 9 + 6 bits;
    72 -> 72 + 8 bits)"""
    out = []
    while len(out) < n:
        s = wcode.decode(bits)
        if s < REP_LEN:
            out.append(s)
            continue
        if s == REP_ZERO:
            r = 2 + bits.read(3)
            if r == 9:
                r += bits.read(6)
                if r == 72:
                    r += bits.read(8)
            v = 0
        else:
            r = 2 + bits.read(2)
            if r == 5:
                r += bits.read(4)
                if r == 20:
                    r += bits.read(8)
            if not out:
                raise ValueError('a repeat cannot start a table')
            v = out[-1]
        if len(out) + r > n:
            raise ValueError('repeat past the end of the table')
        out += [v] * r
    return out


def read_weight_code(bits):
    lengths = read_lengths_plain(bits, 15, 7)
    if check_code(lengths, 7) <= 0:
        raise ValueError('empty weight code')
    return Code(lengths)


def read_table(bits, wcode, n, cap):
    lengths = read_lengths_by_code(bits, wcode, n)
    check_code(lengths, cap)
    return Code(lengths)

# ------------------------------------------------------------------------------------------- literal stream (4)

def decode_literals(data, pos, count):
    """count literals in blocks of 32768; returns (literals, position after the stream)"""
    out = bytearray()
    code = None                                          # the code of the last block of type 1
    while len(out) < count:
        size = min(32768, count - len(out))
        if pos >= len(data):
            raise ValueError('literal block past the input')
        kind = data[pos]
        if kind == 0:                                    # stored
            if pos + 1 + size > len(data):
                raise ValueError('stored literals past the input')
            out += data[pos + 1:pos + 1 + size]
            pos += 1 + size
            continue
        if kind > 2:
            raise ValueError('reserved literal block type')
        body_size = int.from_bytes(data[pos + 1:pos + 3], 'little')
        bits = Bits(data, pos + 3)
        if kind == 1:                                    # a code of its own
            wcode = read_weight_code(bits)
            lengths = read_lengths_by_code(bits, wcode, 256)
            if check_code(lengths, 12) <= 0:
                raise ValueError('empty literal code')
            code = Code(lengths)
        elif code is None:                               # 2: the code of the last block of type 1
            raise ValueError('a literal block reuses a code before any')
        body = bits.align()
        end = body + body_size
        if end > len(data):
            raise ValueError('literal block past the input')
        if size >= 512:
            e = [int.from_bytes(data[body + 2 * k:body + 2 * k + 2], 'little') for k in range(3)]
            if not e[0] <= e[1] <= e[2] <= body_size - 6:
                raise ValueError('bad stream sizes')
            starts = [body + 6, body + 6 + e[0], body + 6 + e[1], body + 6 + e[2]]
            q = (size >> 4) << 2
            counts = [q, q, q, size - 3 * q]
        else:
            starts, counts = [body], [size]
        for st, cnt in zip(starts, counts):
            b = Bits(data, st)
            for _ in range(cnt):
                out.append(code.decode(b))
        pos = end
    return bytes(out), pos

# ---------------------------------------------------------------------------- values with extra bits (5.4, 6.3)

def ext_value(sym, bits):
    """WZIP_L literal-run symbols 32-70 and length values 32-4095: (high part, extra bits) per symbol"""
    k = sym - 32
    if k < 16:
        hi, n = 16 + k, 1
    elif k < 24:
        hi, n = 8 + (k - 16), 3
    elif k < 28:
        hi, n = 4 + (k - 24), 5
    elif k < 32:
        hi, n = 4 + (k - 28), 6
    elif k < 34:
        hi, n = 2 + (k - 32), 8
    elif k < 36:
        hi, n = 2 + (k - 34), 9
    elif k < 38:
        hi, n = 2 + (k - 36), 10
    elif k == 38:
        hi, n = 0, 24
    else:
        raise ValueError('value symbol out of range')
    return hi << n | bits.read(n)


def range_value(sym, bits):
    """WZIP_M and WZIP_S value codes 0-31: 0-7 direct; then ranges with raw low bits"""
    if sym < 8:
        return sym
    for msb, (base, shift) in enumerate(zip([0, 0, 0, 8, 12, 16, 20, 22, 24, 25, 26, 27, 28, 29, 30, 31],
                                            [0, 0, 0, 1, 2, 3, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15])):
        if msb < 3:
            continue
        ncodes = 1 << (msb - shift)
        if base <= sym < base + ncodes:
            return (1 << msb) + ((sym - base) << shift) + bits.read(shift)
    raise ValueError('value symbol out of range')


def offset_value(sym, bits):
    """an offset symbol >= 4: (2 + its low bit) << e, plus e raw bits, e = sym / 2 - 1"""
    e = (sym >> 1) - 1
    return ((2 | (sym & 1)) << e) | bits.read(e)

# --------------------------------------------------------------------------------------------- matches (5.6)

def copy_match(out, dictionary, off, length):
    if off < 1 or off > len(out) + len(dictionary):
        raise ValueError('offset before the history')
    for _ in range(length):
        j = len(out) - off
        out.append(out[j] if j >= 0 else dictionary[len(dictionary) + j])

# ------------------------------------------------------------------------------------------------- WZIP_L (5)

def off_widths(n):
    """the base widths of lengths 3..8 for an input of n bytes, the widest raised to cover the input"""
    rows = [(28, (10, 15, 20, 24, 25, 26)), (26, (11, 15, 19, 23, 25, 26)), (25, (11, 15, 19, 22, 24, 24)),
            (24, (12, 16, 20, 22, 23, 23)), (23, (12, 16, 19, 22, 22, 22)), (22, (12, 15, 18, 21, 21, 21)),
            (21, (12, 15, 17, 20, 20, 20)), (20, (12, 15, 17, 19, 19, 19)), (19, (12, 15, 17, 18, 18, 18)),
            (18, (12, 15, 17, 17, 17, 17)), (17, (12, 15, 16, 16, 16, 16)), (15, (13, 14, 15, 15, 15, 15))]
    w = [0] * 9
    for shift, row in rows:
        if n >> shift:
            w[3:9] = row
            break
    if w[8]:
        top, x = w[8], w[8]
        while x < 27 and (1 << x) - 3 < n:
            x += 1
        for i in range(3, 9):
            if w[i] == top:
                w[i] = min(x, 26) if i < 8 else x
    return w


def offset_groups(width, fine):
    """(5.3) the group of each length symbol 0-67, and each group's alphabet size"""
    i = 8
    while i > 0 and width[i] == width[i - 1]:
        i -= 1
    starts = [0, 1, 2, 3, 4, 5, 7, 13] if fine else list(range(min(i - 2, 8)))
    group_of, sizes = [0] * 68, []
    for g, s in enumerate(starts):
        end = starts[g + 1] if g + 1 < len(starts) else 68
        for m in range(s, end):
            group_of[m] = g
        last_len = end - 1 + 3
        sizes.append(2 * width[min(8, last_len)] if last_len < 32 else 2 * width[8])
    return group_of, sizes


RUN_SYM = 67


def decode_l(data, n, dictionary=b''):
    """a WZIP_L payload (after the wrapper's size field) of n >= 32768 decoded bytes; the windows are those of the
    history, the dictionary and the output (5.2, 8)"""
    width = off_widths(n + len(dictionary))
    if len(data) < 3 or data[2] >> 4 > 1:
        raise ValueError('bad window header')
    fine = data[2] >> 4
    gaps = [data[0] & 15, data[0] >> 4, data[1] & 15, data[1] >> 4, data[2] & 15]
    for k in range(3, 8):
        width[k] = width[8] - gaps[k - 3]
    if width[3] < 4 or any(width[k] < width[k - 1] for k in range(4, 9)):
        raise ValueError('windows must widen with the length')
    group_of, gsize = offset_groups(width, fine)
    pos = 3
    nlen = 4 if n >> 16 else 2
    nlits = int.from_bytes(data[pos:pos + nlen], 'little')
    pos += nlen
    if nlits > n:
        raise ValueError('more literals than output')
    lits, pos = decode_literals(data, pos, nlits)
    lp = 0
    out = bytearray()
    cache = [0x7F7F7F7F] * 4
    lr_code, joint, joint_sj, off_codes = None, None, None, [None] * len(gsize)    # the codes last sent
    while len(out) < n:                                  # sequence blocks (5.4)
        bits = Bits(data, pos)
        slot_joint = bits.read(1)
        reuse = [bits.read(1) for _ in range(2 + len(gsize))]
        if (reuse[0] and lr_code is None) or (reuse[1] and joint_sj != slot_joint) or                 any(reuse[2 + g] and off_codes[g] is None for g in range(len(gsize))):
            raise ValueError('a sequence block reuses a code not sent before')
        if not all(reuse):
            wcode = read_weight_code(bits)
            if not reuse[0]:
                lr_code = read_table(bits, wcode, 71, 11)
            if not reuse[1]:
                joint, joint_sj = read_table(bits, wcode, 1020 if slot_joint else 204, 11), slot_joint
            for g in range(len(gsize)):
                if not reuse[2 + g]:
                    off_codes[g] = read_table(bits, wcode, gsize[g], 10)
        pos = bits.align()                               # the size of stream A, then A (sequences 0, 2, ...), B (1, 3, ...)
        size_a = int.from_bytes(data[pos:pos + 3], 'little')
        streams = [Bits(data, pos + 3), Bits(data, pos + 3 + size_a)]
        done = False
        for k in range(16384):
            bits = streams[k & 1]
            j = joint.decode(bits)
            if slot_joint:
                slot, cls, m = j // 204, j // 68 % 3, j % 68
            else:
                slot, cls, m = 4, j // 68, j % 68
            if cls < 2:
                lit = cls
            else:
                lit = lr_code.decode(bits)
                if lit >= 32:
                    lit = ext_value(lit, bits)
            if lit > n - len(out) or lp + lit > len(lits):
                raise ValueError('literal run past the output or the literals')
            if lit == n - len(out):                      # the last literal run ends the output
                out += lits[lp:lp + lit]
                lp += lit
                done = True
                break
            out += lits[lp:lp + lit]
            lp += lit
            if not slot_joint or slot == 4:
                sym = off_codes[group_of[m]].decode(bits)
                if slot_joint and sym < 4:
                    raise ValueError('a cache slot in the offset field of a slot-joint block')
            else:
                sym = slot
            if sym >= 4:
                v = offset_value(sym, bits) - 3
                if m == RUN_SYM:                         # a run of the preceding byte, v copies
                    if v > n - len(out):
                        raise ValueError('run past the output')
                    copy_match(out, dictionary, 1, v)
                    continue
                off = v
                cache = [off] + cache[:3]
            else:
                if m == RUN_SYM:
                    raise ValueError('a run needs an explicit count')
                off = cache[sym]
                cache = [off] + cache[:sym] + cache[sym + 1:]
            length = m + 3
            if length >= 32:
                length = ext_value(length, bits)
            if length > n - len(out):
                raise ValueError('match past the output')
            copy_match(out, dictionary, off, length)
        if streams[0].align() != pos + 3 + size_a:
            raise ValueError('stream A is not of its stated size')
        pos = streams[1].align()
        if pos > len(data):
            raise ValueError('sequence block past the input')
        if done:
            break
    if len(out) != n:
        raise ValueError('decoded %d bytes, expected %d' % (len(out), n))
    if lp != nlits:
        raise ValueError('literals left over')
    return bytes(out)

# ------------------------------------------------------------------------------------------------- WZIP_M (6)

def decode_m(data, n, dictionary=b''):
    """a WZIP_M payload (exactly its bytes) of n < 32768 decoded bytes"""
    nlits = int.from_bytes(data[0:2], 'little')
    if nlits > n:
        raise ValueError('more literals than output')
    lits, pos = decode_literals(data, 2, nlits)
    bits = Bits(data, pos)
    wcode = read_weight_code(bits)
    lr_code = read_table(bits, wcode, 32, 9)
    len_code = read_table(bits, wcode, 28, 9)
    off_codes = [read_table(bits, wcode, 26, 9), read_table(bits, wcode, 30, 9)]
    streams = [Bits(data, bits.align()), Bits(bytes(reversed(data)), 0)]    # A forward, B from the end backward
    out, lp, cache, k = bytearray(), 0, [0xFFFFFFFF] * 4, 0
    while True:
        b = streams[k & 1]
        k += 1
        lit = range_value(lr_code.decode(b), b)
        if lit > n - len(out) or lp + lit > len(lits):
            raise ValueError('literal run past the output or the literals')
        out += lits[lp:lp + lit]
        lp += lit
        if len(out) == n:                                # the terminal record
            break
        m = len_code.decode(b)
        sym = off_codes[min(m, 1)].decode(b)
        if sym >= 4:
            off = offset_value(sym, b) - 3
            cache = [off] + cache[:3]
        else:
            off = cache[sym]
            cache = [off] + cache[:sym] + cache[sym + 1:]
        length = range_value(m + 3, b)
        if length > n - len(out):
            raise ValueError('match past the output')
        copy_match(out, dictionary, off, length)
    if lp != nlits:
        raise ValueError('literals left over')
    if streams[0].byte_pos() + streams[1].byte_pos() != len(data):
        raise ValueError('streams A and B do not fill the payload')
    return bytes(out)

# --------------------------------------------------------------------------------------------- wrapper (4.1)

def decode(stream, dictionary=b''):
    """a wzip_compress stream; a WZIP_L one with the dictionary, if any (wzip_compress_usingDict, linked blocks)"""
    if len(stream) < 2:
        raise ValueError('truncated size field')
    h = int.from_bytes(stream[0:2], 'little')
    if h >> 15:
        n, pos = (h & 0x7FFF) | int.from_bytes(stream[2:4], 'little') << 15, 4
    else:
        n, pos = h, 2
    if n == 0:
        return bytes(stream[2:])                         # stored
    return decode_l(stream[pos:], n, dictionary) if n >= 32768 else decode_m(stream[pos:], n)

# ------------------------------------------------------------------------------------------------- WZIP_S (7)

def decode_s(block, dictionary=b''):
    if not block or block[0] >> 5:
        raise ValueError('bad header')
    mode, cls, use_dict = block[0] & 3, (block[0] >> 2) & 3, (block[0] >> 4) & 1
    if mode == 3 or (use_dict and mode != 2):
        raise ValueError('bad header')
    if cls < 3:
        n, pos = (4096, 8192, 16384)[cls], 1
    else:
        n, pos = 1 + int.from_bytes(block[1:3], 'little'), 3
    if mode == 0:
        if len(block) != pos + n:
            raise ValueError('stored block of the wrong size')
        return bytes(block[pos:])
    if mode == 1:
        if len(block) != pos + 1:
            raise ValueError('fill block of the wrong size')
        return bytes(block[pos:pos + 1]) * n
    hist = dictionary[-32767:] if use_dict else b''
    if use_dict and not dictionary:
        raise ValueError('a dictionary is required')
    body = block[pos:]
    w_block = 1
    while (1 << w_block) < n:
        w_block += 1
    w_full = w_block
    while (1 << w_full) < n + len(hist):
        w_full += 1
    n_off = 4 if w_full <= 1 else 2 * w_full + 1
    main = Bits(body, 0)
    wcode = read_weight_code(main)
    lit_lengths = read_lengths_by_code(main, wcode, 256)
    lit_max = check_code(lit_lengths, 10)
    lit_code = Code(lit_lengths)
    lr_code = read_table(main, wcode, 32, 9)
    if lr_code.max == 0:
        raise ValueError('empty literal-run code')
    len_code = read_table(main, wcode, 30, 9)
    off_code = read_table(main, wcode, n_off, 9)
    nlits = main.read(w_block + 1)
    if nlits > n or (nlits and not lit_max):
        raise ValueError('bad literal count')
    back = Bits(bytes(reversed(body)), 0)                # stream B, read from the block end backward
    lits = [0] * nlits
    four = w_block >= 12                                 # four literal streams in blocks above 2 KB
    if four:
        q = nlits >> 2
        mid = len(body) - main.read(w_block)             # the point P: streams D and B lie after it
        if mid < 0:
            raise ValueError('the point P lies before the block')
        c = Bits(bytes(reversed(body[:mid])), 0)         # stream C, read backward from P
        d = Bits(body, mid)                              # stream D, read forward from P
        for i in range(q):
            lits[i] = lit_code.decode(c)
        for i in range(q, 2 * q):
            lits[i] = lit_code.decode(d)
        a_first, a_count = 2 * q, q
    else:
        a_first, a_count = 0, (nlits + 1) // 2
    for i in range(a_first, a_first + a_count):
        lits[i] = lit_code.decode(main)
    for i in range(a_first + a_count, nlits):
        lits[i] = lit_code.decode(back)
    out, lp, rep = bytearray(), 0, [1, 4, 8]
    while True:
        lit = range_value(lr_code.decode(main), main)
        if lit > n - len(out) or lp + lit > nlits:
            raise ValueError('literal run past the output or the literals')
        out += bytes(lits[lp:lp + lit])
        lp += lit
        if len(out) == n:
            if lp != nlits:
                raise ValueError('literals left over')
            break
        if len_code.max == 0:
            raise ValueError('empty length code')
        s = len_code.decode(main)
        length = 2 if s < 2 else range_value(s + 1, main)
        if s == 0:
            raise ValueError('raw length-2 matches are disabled in this build (S_W2_BITS = 0)')
        if s == 1:
            off = rep[0]                                 # length 2 at repeat slot 0: no update
        else:
            if off_code.max == 0:
                raise ValueError('empty offset code')
            sym = off_code.decode(back)
            v = sym if sym < 4 else offset_value(sym, back)
            if v >= 3:
                off = v - 2
                rep = [off, rep[0], rep[1]]
            elif v == 1:
                off = rep[1]
                rep = [rep[1], rep[0], rep[2]]
            elif v == 2:
                off = rep[2]
                rep = [rep[2], rep[0], rep[1]]
            else:
                off = rep[0]
        if length > n - len(out) or off > len(out) + len(hist):
            raise ValueError('match past the output or before the history')
        copy_match(out, hist, off, length)
    if four:
        if main.byte_pos() + c.byte_pos() != mid or d.byte_pos() + back.byte_pos() != len(body):
            raise ValueError('the four streams do not fill the block')
    elif main.byte_pos() + back.byte_pos() != len(body):
        raise ValueError('the main stream and stream B do not fill the block')
    return bytes(out)


if __name__ == '__main__':
    args = sys.argv[1:]
    d = b''
    if '--dict' in args:
        i = args.index('--dict')
        d = open(args[i + 1], 'rb').read()
        del args[i:i + 2]
    is_s = '--s' in args
    if is_s:
        args.remove('--s')
    data = open(args[0], 'rb').read()
    out = decode_s(data, d) if is_s else decode(data)
    if len(args) > 1:
        open(args[1], 'wb').write(out)
    else:
        print('%d bytes' % len(out))
