# WLZ4 block format

This document specifies the compressed format of WLZ4, the byte-aligned, LZ4-class codec of this repository
(`src/WLZ4.h`, `src/WLZ4.c`), precisely enough to write an independent decoder. Sections 1-7 are normative; sections
8-10 describe the reference implementation and are informative.

`doc/wlz4_decode.py` is an independent decoder written from this document alone. It decodes the output of every
encoder level of `src/WLZ4.c` (fast, lazy, hash-chain levels 0-7, optimal levels 8-12, and dictionary compression)
identically, which is how the specification was checked.

Format version: the revision of October 2026 (repository commit `b974128`). A block carries no version field, and
blocks of the earlier format (section 11) cannot be told apart from current ones: store the format version
alongside the data if both may occur.

## 1. Overview

WLZ4 is an LZ77 format in the style of LZ4: a block is a sequence of *sequences*, each a run of literal bytes
followed by a match that copies earlier output. It differs from LZ4 in three ways:

- **Length-dependent offset fields.** The size of a match's offset field depends on the match length and, for
  lengths 4 and up, on a one-bit flag: a length-3 match reaches 256 bytes back, lengths 4-5 reach 128 bytes with a
  one-byte field or 32 KiB with a two-byte one, and longer matches reach 32 KiB with two bytes or 8 MiB with three.
- **Minimum match length 3**, and match lengths up to 17 without an extension byte.
- **A size header**: a block begins with its decoded size, so a decoder can allocate its output, and decoding can
  be checked against a known end.

A block holds one whole input; there is no frame format, no checksum and no block chaining (a dictionary,
section 7, provides history before the input).

## 2. Conventions

- Multi-byte integers are little-endian.
- `n` is the decoded size of the block, in bytes. Positions in the output are numbered from 0.
- A match `(length, offset)` at output position `p` copies the bytes at positions `p - offset`, `p - offset + 1`, ...,
  `p - offset + length - 1` to positions `p`, ..., `p + length - 1`, **one byte at a time in increasing order**: when
  `offset < length` the match overlaps its own output and repeats a pattern of `offset` bytes.

## 3. Block layout

```
size header | sequence | sequence | ... | last sequence (literals and the end marker)
```

### 3.1 Size header

| Decoded size `n` | Header | Bytes |
|---|---|---|
| `n < 32768` | `u16` = `n` (top bit 0) | 2 |
| `32768 <= n <= 0x7FFFFF00` | `u16` = `0x8000 \| (n & 0x7FFF)`, then `u16` = `n >> 15` | 4 |

The top bit of the first `u16` selects the form. An encoder must use the 2-byte form when `n < 32768`. The largest
block is `0x7FFFFF00` bytes (`WLZ_MAX_INPUT_SIZE`).

### 3.2 Sequences

Sequences follow the header until the end marker (section 5). Each sequence is:

```
token | [literal-run extension] | literals | offset field | [match-length extension]
```

## 4. Sequence fields

### 4.1 Token

One byte. The high nibble `L` (0-15) gives the literal run; the low nibble `c` (0-15) is the **match code**.

### 4.2 Literal run

If `L < 15`, the run has `L` literals. If `L = 15`, an extension (section 4.3) follows the token and the run has
`15 + e` literals. The literals follow, copied verbatim to the output.

### 4.3 Extensions

An extension encodes a value `e >= 0`:

| First byte `b` | Value | Bytes |
|---|---|---|
| `0..251` | `e = b` | 1 |
| `252..255` | `m = b - 251` (1-4); `e = 252 + v`, where `v` is the next `m` bytes as an unsigned little-endian integer | `1 + m` |

Encoders use the shortest form: one byte for `e < 252`, else the smallest `m` with `e - 252 < 256^m`. (A decoder
need not check this.)

### 4.4 Match code, offset field and match length

| Code `c` | Match length | Offset field | Offset range |
|---|---|---|---|
| 0 | 3 | 1 plain byte: offset = byte | 1-255 |
| 1, 2 | 4, 5 | flagged, base 1 byte: 1 byte if flag 0, 2 bytes if flag 1 | 1-127, or 1-32767 |
| 3-14 | 6-17 | flagged, base 2 bytes: 2 bytes if flag 0, 3 bytes if flag 1 | 1-32767, or 1-8388607 |
| 15 | 18 + `e` (extension after the offset field) | as codes 3-14 | as codes 3-14 |

A **flagged offset field** is a little-endian integer `v` of `k` bytes whose least significant bit is the flag:
the flag is bit 0 of the field's first byte, so it is known before the rest of the field is read. Then

- `k = 1 + [c > 2] + flag` bytes, where `[c > 2]` is 1 for codes 3-15 and 0 for codes 1-2;
- offset `= v >> 1` (7, 15 or 23 bits).

With code 0 the field is one plain byte (no flag). So for every code: **offset bytes = 1 if `c = 0`, else
`1 + [c > 2] + flag`**.

An encoder may write the long form (flag 1) for an offset that the short form could hold; the reference encoders
always write the shortest form. Code 15 is used exactly for lengths 18 and up; a length from 4 to 17 uses code
`length - 3`.

### 4.5 Matches

After the offset field (and, for code 15, the extension), the decoder copies `length` bytes from `offset` bytes back
(section 2). A valid match has `1 <= offset <= p + D`, where `p` is the number of bytes decoded so far in this block
and `D` the dictionary size (0 without one): it may reach into the dictionary (section 7) but not before it. A match
may end exactly at the end of the output; there is no restriction on the last bytes of a block (unlike LZ4's
"last five bytes are literals" rule).

## 5. End of block

A sequence whose offset is **0** ends the block: its literals are output, and no match is copied. Encoders write the
end marker as a token with match code 0 (the low nibble 0) followed, after the literals, by a single zero byte. A
decoder must also accept a zero offset with codes 1-14 as the end (the reference decoders do), and must treat a zero
offset with code 15 as corrupt.

After the end marker the decoder has produced exactly `n` bytes, or the block is corrupt. The end marker is the last
byte of the block; the size of a compressed block is known to the application, which stores it (the reference
decoders ignore any bytes after the end marker; `doc/wlz4_decode.py` rejects them).

An empty input is the 4-byte block `00 00 00 00`: the header, a token with no literals and code 0, and the zero
offset.

## 6. Validity

A conforming decoder must reject (and the checked reference decoder `WLZ_Decompress` does reject) a block in which:

- the header, a literal run, an extension or an offset field extends past the end of the input;
- the input ends before an end marker;
- an offset is 0 with code 15, or exceeds the bytes decoded so far plus the dictionary size;
- the output would exceed `n` bytes, or ends with fewer.

## 7. Dictionary

A dictionary is a byte string `dict` that precedes the input: offsets may reach `D = len(dict)` bytes before the
first output byte, and output position `-j` (1 <= j <= D) is `dict[D - j]`. A match may start in the dictionary and
continue into the output. The format does not identify the dictionary; the decoder must be given the same one
(`WLZ_Compress_wDict` / `WLZ_Decompress_wDict`). Only offsets up to 8388607 can be coded, so at most the last 8 MiB
of a dictionary are reachable.

## 8. Worked example

The 32-byte block below decodes to the 61 bytes
`abcabcabcabcXYZabcdefghijkabcdefghijkhijkabcabcabcabcXYZabcde`. It was written by hand from this document and is
decoded identically by `WLZ_Decompress`, `WLZ_Decompress_Trusted` and `doc/wlz4_decode.py`.

| Bytes | Meaning | Output so far |
|---|---|---|
| `3D 00` | size header: `n` = 61 | |
| `36` | token: 3 literals, code 6 (length 9) | |
| `61 62 63` | literals `abc` | `abc` |
| `06 00` | flagged field `0x0006`: flag 0, 2 bytes, offset 3 | match (9, 3) overlaps: `abcabcabcabc` (12) |
| `30` | token: 3 literals, code 0 (length 3) | |
| `58 59 5A` | literals `XYZ` | ...`XYZ` (15) |
| `0F` | plain offset 15 | match (3, 15): `abc` (18) |
| `88` | token: 8 literals, code 8 (length 11) | |
| `64 ... 6B` | literals `defghijk` | (26) |
| `16 00` | flagged field `0x0016`: flag 0, offset 11 | match (11, 11): `abcdefghijk` (37) |
| `01` | token: 0 literals, code 1 (length 4) | |
| `08` | flagged field `0x08`: flag 0, 1 byte, offset 4 | match (4, 4): `hijk` (41) |
| `0F` | token: 0 literals, code 15 | |
| `52 00` | flagged field `0x0052`: flag 0, offset 41 | |
| `02` | extension 2: length 18 + 2 = 20 | match (20, 41): `abcabcabcabcXYZabcde` (61) |
| `00 00` | token (0 literals, code 0), offset 0 | end of block, 61 bytes as stated |

Two forms the example does not show: a length-5 match at offset 200 needs the long form of its field, flag 1, two
bytes: `200 << 1 | 1 = 0x191` is written `91 01`; a length-6 match at offset 100000 writes
`100000 << 1 | 1 = 0x30D41` as `41 0D 03`.

## 9. Reference decoder (informative)

`src/WLZ4.c` has two decoders with the same output on valid input:

- `WLZ_Decompress` (default) checks every rule of section 6 and never reads outside the input or writes outside the
  output buffer, whatever the input (fuzzed with sanitizers). It requires `dstCapacity >= n + WLZ_MEM_OVERHEAD`
  (32): copies may write up to 31 bytes past the end of the output.
- `WLZ_Decompress_Trusted` skips the checks, for input known to come from a WLZ4 encoder; it may read up to
  `WLZ_TRUSTED_SRC_PAD` (32) bytes past the input. On Silesia it is 9-15% faster.

Techniques used (none is required by the format):

- **One load per sequence.** The decoder reads 4 bytes `v` at the offset field. Two 16-entry tables give, for each
  code, the flag bit `f = [c > 0]` and the base `b = [c > 2]`; the field has `1 + b + (v & f)` bytes and the offset
  is `(v >> f)` masked to its width, with no branch on the code. Unless an extension follows, the next token is the
  byte of `v` after the field, so the critical path of a sequence holds one load. (Compares instead of the tables,
  or a three-term sum on that path, cost about a tenth of the decoding speed.)
- **Wild copies.** Literal runs up to 14 and matches up to 17 bytes are copied with fixed 16-byte and 8-byte moves;
  longer matches with 32-byte moves (offset >= 16) or 8-byte moves (offset >= 8); offsets below 8 by pattern copies.
- **Margins.** The main loop runs while 64 bytes of input and 32 of output remain, so that only long literal runs
  and long matches check lengths; the last sequences are decoded by an exact loop (`WLZ_Decode_Tail`).

## 10. Reference encoders (informative)

All encoders write the shortest field for every match and write a sequence only if the output (excluding the header)
stays within 4 bytes of the input it covers; otherwise the bytes are kept as literals. Incompressible input therefore
grows by at most 15 bytes including the header (LZ4: about `n/255`). The output buffer must hold
`WLZ_COMPRESSBOUND(n)` = `n + 24` bytes.

| API | Levels | Match finding |
|---|---|---|
| `WLZ_Compress_Fast(..., acceleration)` | fast | two hash tables (3-byte hash, 2^12 entries; 5-byte hash, 2^15); takes a candidate of at least 3 bytes within 256 B, 5 within 32 KiB or 6 within 8 MiB; skips faster on incompressible data |
| `WLZ_Compress` | lazy | the same, with one step of lazy evaluation and a check of the last offset |
| `WLZhc_Compress(..., 0..7)` | hash chain | a 5-byte hash chain over 64 KiB walked 1, 2, 4, ..., 128 steps; candidates compared by length less offset bytes beyond two; probes of the far window (the previous position of the same 5-byte hash, uncapped, and of the same 8 bytes in a table of up to 2^20 entries) |
| `WLZhc_Compress(..., 8..12)` | optimal | a 3-byte chain (256 B), a 4-byte chain (64 KiB) and a 6-byte chain (8 MiB); one candidate per offset class (within 128 B, 256 B, 32 KiB, 64 KiB, and beyond), each length priced with the cheapest candidate that codes it; segments of up to 4096 positions priced in exact bytes |

Optimal-level parameters (chain steps in the 64 KiB, 256 B and far windows; a match this long is taken at once):

| Level | 64 KiB | 256 B | far | sufficient length |
|---|---|---|---|---|
| 8 | 16 | 8 | 8 | 32 |
| 9 | 32 | 8 | 16 | 64 |
| 10 | 64 | 16 | 32 | 64 |
| 11 | 256 | 32 | 64 | 128 |
| 12 | 4096 | 64 | 256 | 4096 |

The far chain dominates the time of the optimal levels; their encoder memory is about 40 MB (the 8 MiB chain).
`WLZ_Compress_wDict` compresses with a dictionary (section 7); the hash-chain and optimal levels do not use one.
Measurements are in the paper (`papers/WZIP_WLZ4_DCC.pdf`) and in `results/`.

## 11. Earlier format

Until October 2026 the match codes were: 0-1 for lengths 3-4 with a one-byte offset (window 256); 2-14 for lengths
4-16, and 15 for length 17 plus an extension, all with a flagged field of 2 or 3 bytes (32 KiB or 8 MiB). The header,
tokens, literal runs, extensions and end marker were as above. The revision gives lengths 4-5 a flagged one- or
two-byte field and one size rule for every code; it compresses Silesia 0.2-0.6% better at every level and decodes as
fast. Blocks of the two formats are not interchangeable and carry no marker that tells them apart.
