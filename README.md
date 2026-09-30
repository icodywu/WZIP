# WZIP and WLZ4

Two LZ77 codecs built on **match-length-dependent sliding windows**: each match length has its own maximum
distance, so short matches use short, cheap offsets while long matches reach the whole history. Lengths are decoded
before offsets, so the decoder knows each match's window and no extra field is sent.

- **WZIP** is an entropy-coded, Zstandard-class codec (Huffman coding only).
- **WLZ4** is a byte-aligned, LZ4-class codec: one-byte offsets for lengths 3 and 4, and flagged two- or three-byte
  offsets (32 KB or 8 MB) for longer matches.

Both streams begin with the input length, from which WZIP derives its window schedule and a decoder sizes its output.
Incompressible input grows by at most 2 bytes with WZIP and 15 with WLZ4.

## Results

Single thread, Intel Core i7-8850H, GCC 14.2 `-O2`; each file compressed whole. Ratio is total input over total
output; speeds in MB/s. Full tables, the harness and the raw outputs are in [`bench/`](bench) and [`results/`](results).
WZIP and WLZ4 decode in their trusted mode here, as in the paper (see Usage); their default, bounds-checked decoders
are 6-10% (WLZ4) and at most 8% (WZIP) slower on Silesia.

| Silesia (212 MB, 12 files) | Ratio | Compress | Decompress |
|---|---:|---:|---:|
| LZ4HC 12 | 2.743 | 8.95 | 2991 |
| **WLZ4 2** | 2.748 | 44.9 | 2586 |
| **WLZ4 12** | 3.179 | 1.16 | 2235 |
| Zstandard 19 | 4.005 | 2.57 | 1006 |
| Zstandard 22 | 4.045 | 1.93 | 967 |
| **WZIP 11** | 4.080 | 1.40 | 812 |
| **WZIP 13** | 4.097 | 0.62 | 752 |
| Brotli 11 | 4.276 | 0.47 | 383 |
| xz -9e | 4.374 | 1.90 | 119 |

| enwik9 (1 GB of Wikipedia) | Ratio | Compress | Decompress |
|---|---:|---:|---:|
| Zstandard 22 | 4.676 | 1.25 | 675 |
| xz -9e | 4.722 | 1.34 | 136 |
| **WZIP 11** | 4.745 | 0.92 | 492 |
| **WZIP 13** | 4.759 | 0.42 | 484 |

### Independent 4 KB and 8 KB blocks

Storage pages and key-value stores compress small blocks independently. Here every Silesia file is cut into 4 KB
or 8 KB blocks, each compressed and decompressed by its own call (`bench/bench_blocks.c`; results for
Canterbury+Calgary are in [`results/blocks_C.txt`](results/blocks_C.txt)).

| Silesia in blocks | 4 KB ratio | Compress | Decompress | 8 KB ratio | Compress | Decompress |
|---|---:|---:|---:|---:|---:|---:|
| LZ4 | 1.727 | 539 | 2564 | 1.824 | 505 | 2727 |
| LZ4HC 12 | 1.856 | 33.6 | 2856 | 2.004 | 30.2 | 3107 |
| **WLZ4 2** | 1.893 | 62.7 | 2347 | 2.020 | 71.1 | 2647 |
| **WLZ4 10** | 1.940 | 20.6 | 1610 | 2.081 | 24.9 | 2376 |
| Zstandard 1 | 2.315 | 210 | 571 | 2.476 | 251 | 699 |
| Zstandard 9 | 2.424 | 33.3 | 599 | 2.619 | 28.4 | 693 |
| Zstandard 19 | 2.522 | 3.88 | 530 | 2.737 | 3.80 | 636 |
| **WZIP_S 1** | 2.434 | 52.3 | 314 | 2.580 | 59.3 | 410 |
| **WZIP_S 5** | 2.499 | 31.7 | 330 | 2.680 | 30.8 | 424 |
| **WZIP_S 9** | 2.500 | 24.2 | 332 | 2.682 | 17.6 | 424 |
| **WZIP_M 12** | 2.502 | 0.82 | 379 | 2.718 | 0.51 | 479 |

WZIP_S 1 compresses 5% more than Zstandard 1 on 4 KB blocks, and WZIP_S 5 3% more than Zstandard 9 at the same
compression speed, but WZIP_S decodes at 55-61% of Zstandard's speed, and Zstandard 19 still compresses 1-2% more
than WZIP_S 9. WLZ4 compresses 1-4.5% more than LZ4HC 12, decoding 15-44% slower. WZIP_M, reached through
`wzip_compress`, compresses slowly on small blocks because the one-call interface builds its tables on every call;
WZIP_S keeps them in a reusable context.

## Papers

- Y. Wu, "WZIP and WLZ4: LZ77 codecs with match-length-dependent sliding windows," submitted to the 2027
  Data Compression Conference. Describes both codecs, their optimal parser and the measurements above.
- Y. Wu, "Improved LZ77 compression with match-length-dependent sliding windows," submitted to IEEE Transactions on
  Information Theory. Analyzes the scheme: universality of greedy parsing, schedule calibration, run and period gains.
- Y. Wu, "Improved LZ77 compression," in Proc. Data Compression Conference, 2021, p. 377. Introduces the scheme.

## Build and test

```sh
make          # build/libwzip.a and build/roundtrip
make test     # round trip of every codec and level on synthetic inputs, with guard checks on every buffer
make check FILES="file1 file2"
make fuzz     # damaged streams of every codec, under AddressSanitizer and UndefinedBehaviorSanitizer (Linux, macOS)
```

The sources (`src/`) are C99 and build without warnings (`-Wall`) with GCC 14.2 (MinGW-w64, Windows), and GCC 11.4
and clang 14 (Ubuntu 22.04, x86-64), where the tests also pass under AddressSanitizer and UndefinedBehaviorSanitizer.
On x86 the decoders pick BMI2 code paths at run time.

The default decoders validate their input, as LZ4's safe decoder does: whatever the stream, a decoder reads nothing
outside the compressed buffer (and the dictionary) and writes nothing outside the output buffer; a corrupt or truncated
stream returns 0. `make fuzz` decodes flipped, overwritten, truncated, spliced and extended streams of every codec from
buffers of exactly their size, so that the sanitizers catch any stray access; it also decodes each undamaged stream
from such a buffer, and in trusted mode from a buffer with exactly the documented slack.

## Usage

```c
#include "WZIP.h"

int cap = WZIP_Cap_CmprSize(n);                       /* input + 256 always suffices */
int cSize = wzip_compress(src, n, dst, &cap, 11);     /* level 0-13; 0 on failure */

int decCap = n;                                       /* the decoded size, also given by WZIP_Read_DecSize */
int dSize = wzip_decompress(dst, cSize, out, &decCap);
```

```c
#include "WLZ4.h"

WLZhc_State_Str* hc = WLZhc_New_State();
unsigned cSize = WLZhc_Compress(hc, src, dst, n, WLZ_COMPRESSBOUND(n), 10);   /* level 0-12 */
unsigned dSize = WLZ_Decompress(dst, out, cSize, n + WLZ_MEM_OVERHEAD);      /* out holds n + WLZ_MEM_OVERHEAD */
WLZhc_Free_State(hc);
```

WZIP_S, for 4 KB and 8 KB blocks, has its own interface (below).

### Trusted mode (opt-in)

The default decoders check every stream (below). For data known to come unmodified from these encoders, such as
data your own program compressed or data verified by a cryptographic MAC, an opt-in trusted mode decodes without any
check. Because every stream begins with its decoded size, it needs nothing from the caller: the output is sized from
the stream, as in the default mode. The compressed buffer must stay readable `WZIP_TRUSTED_SRC_PAD` or
`WLZ_TRUSTED_SRC_PAD` (32) bytes past the stream.

```c
int dSize = wzip_decompress_trusted(dst, cSize, out, &decCap);                         /* WZIP */
unsigned dSize = WLZ_Decompress_Trusted(dst, out, cSize, n + WLZ_MEM_OVERHEAD);        /* WLZ4 */
```

On Silesia the trusted mode decodes 6-10% faster for WLZ4 and up to 8% faster for WZIP. Never use it on data that
may be damaged or crafted: a bad stream can make it read or write out of bounds.

## Limitations

- WZIP_L and WZIP_M keep their window schedule in global state: use them from one thread at a time. WZIP_S and WLZ4
  keep their state in contexts.
- WZIP's optimal levels (7-13) need about 950 MB of encoder memory on a 50 MB input and about 2 GB on enwik9. Inputs
  are limited to 2 GB, and windows to 128 MB.

## Length-2 matches: measured, not used

Apart from WZIP_S's length-2 match at the most recent offset, every match in these codecs has length 3 or more. A
length-2 match with an offset can pay only at very short distances. In WLZ4 it cannot pay at all: it would need an extra token and an offset byte, as many bytes as
the two literals it replaces. In WZIP it was measured with WZIP_S, whose format has a length-2 symbol with raw offset
bits (`S_W2_BITS`), built with 4 bits (distances up to 16) against 0 (off) (`results/blocks_*.txt`, `wzips-L2`):

| WZIP_S ratio | Cant.+Calg. 4 KB | Cant.+Calg. 8 KB | Silesia 4 KB | Silesia 8 KB |
|---|---:|---:|---:|---:|
| level 5 | 2.7968 | 2.9904 | 2.4989 | 2.6796 |
| level 5, length 2 within 16 bytes | 2.7947 | 2.9891 | 2.4978 | 2.6793 |
| level 9 | 2.7979 | 2.9905 | 2.4997 | 2.6816 |
| level 9, length 2 within 16 bytes | 2.7957 | 2.9893 | 2.4986 | 2.6812 |

Length-2 matches lowered the ratio in every case, by 0.01-0.08%: the two literals they replace cost little after
Huffman coding, while each such match adds a sequence. Their effect on decoding speed was within measurement noise.
So no codec here codes length-2 matches at a distance; WZIP_S keeps only a length-2 match at the most recent offset,
which costs one length symbol and no offset bits.

## The three WZIP variants

All three store the decoded size first, so a decoder allocates its output exactly:

| | WZIP_L | WZIP_M | WZIP_S |
|---|---|---|---|
| Input | 32 KB and up, one block (up to 2 GB) | under 32 KB | one block of up to 32 KB, tuned for 4 KB and 8 KB storage pages |
| API | `wzip_compress` / `wzip_decompress`, which pick L or M by size | the same | `WZIPS_compress` / `WZIPS_decompress` |
| Levels | 0 fast; 1–6 greedy and lazy hash chains; 7–13 optimal parsing | 0–12, optimal parsing at 10–12 | 1–9 |
| Windows | derived from the input size: lengths 8+ reach the whole input (up to 128 MB), shorter lengths less; a 3-byte header lets the encoder widen them | fixed: length 3 within 8 KB, lengths 4+ within 32 KB | length 3 within 4 KB; longer lengths the whole block and dictionary |
| Shortest match | 3 | 3 | 2, at the most recent offset only |
| Repeat offsets | 4, move-to-front | 4, move-to-front | 3 |
| Sequence coding | per block, a joint symbol (repeat slot × literal-run class × length) or separate symbols; offset tables per length group (4, or 8 at level 13); run token | offset tables for lengths 3 and 4+; two interleaved sequence streams | one offset table; offsets in a second stream read backward |
| Literals | Huffman, four streams | Huffman, four streams | Huffman, four streams from 8 KB |
| Dictionary | indexed on each call | indexed on each call | prepared once and shared read-only; matches may cross into the input |
| Worst-case output | input + 2 bytes (stored) | input + 2 bytes (stored) | input + 3 bytes |
| Decoder | bounds-checked; trusted mode opt-in | bounds-checked; trusted mode opt-in | bounds-checked |

## Dictionary compression

A dictionary is the history that precedes the input. WZIP gives that history **negative positions**:
dictionary bytes sit at positions `-dictSize … -1`, the input at `0 … n-1`, and an offset is simply the
distance back into this combined history. Where a match's source lies is one sign test:

```c
source = (pos < 0) ? dictEnd + pos : input + pos;
```

### What this design gives you

- **One address space, one test.** Hash tables and chains store history positions. A negative
  position is in the dictionary; there are no base pointers, window limits or index deltas to
  keep consistent.
- **One chain walk.** When a position's previous occurrence lies in the dictionary, its chain link
  simply holds that negative position, and the walk continues through the dictionary's own chain.
  Dictionary and input candidates are searched together, in distance order, by the same loop.
- **Prepare once, share read-only (WZIP_S).** `WZIPS_createCDict` indexes the dictionary once.
  Compressing a block only reads the prepared dictionary: there is no per-block re-indexing, no table
  copy, and one prepared dictionary can serve any number of contexts and threads.
- **Matches may run from the dictionary into the input (WZIP_S)**, exactly as in the concatenated
  history.
- **Safe decoding.** The decoder needs only the dictionary bytes. A block records whether it needs a
  dictionary and fails cleanly without one, and the output never exceeds the stored original size.

### Usage (WZIP_S)

```c
#include "WZIP.h"

WZIPS_CCtx*  cctx  = WZIPS_createCCtx();
WZIPS_CDict* cdict = WZIPS_createCDict(dict, dictSize);   /* built once; its last WZIPS_MAX_DICT bytes are used */

int cSize = WZIPS_compress_usingCDict(cctx, cdict, src, srcSize, dst, WZIPS_COMPRESSBOUND(srcSize), 9);
int dSize = WZIPS_decompress_usingDict(dst, cSize, out, srcSize, dict, dictSize);   /* out needs exactly srcSize bytes */

WZIPS_freeCDict(cdict);
WZIPS_freeCCtx(cctx);
```

WZIP_M and WZIP_L take a dictionary through `WZIP_New_State_M/L` and `WZIP_Decompress_M/L` with the same
negative-position model; they index the dictionary on each call, and their matches stay on one side of
the dictionary end.

### Comparison with zstd

| | WZIP_S | zstd |
|---|---|---|
| Model | dictionary is the history before the input; offsets are distances into it | the same |
| Addressing dictionary positions | negative positions, one sign test | indices relative to a base pointer, with dictionary and low limits; an attached prepared dictionary has its own match state reached through an index delta |
| Searching dictionary and input | one chain walk across both | with an attached dictionary, the input's tables and the dictionary's tables are searched separately |
| Reusing a prepared dictionary | always read-only, never copied or re-indexed | attached read-only for small inputs, copied into the context for larger ones |
| Matches across the dictionary end | yes | yes |
| Identifying the dictionary | one header bit: "dictionary required" | optional dictionary ID of up to 4 bytes |
| Entropy tables in the dictionary | not yet (content only) | yes, in trained dictionaries |
| Dictionary training | not yet | yes (`ZDICT_trainFromBuffer`) |
| Dictionary size used | last 32,767 bytes | up to the window size |

### Measured

Each block of the Silesia files dickens, xml, samba and osdb (first 4 MB of each) was compressed with
the bytes that precede it in the same file as its dictionary: WZIP_S at level 9, and zstd 1.5.6 at
level 19 using the same raw-content dictionary (`ZSTD_compress_usingDict`). Ratio is original size over
compressed size; the gain is the ratio with the dictionary over the ratio without it.

| Block | Dictionary | WZIP_S without → with | WZIP_S gain | zstd without → with | zstd gain |
|------:|-----------:|:---------------------:|------------:|:-------------------:|----------:|
| 4 KB  | 16 KB      | 2.270 → 3.252         | ×1.43       | 2.282 → 3.320       | ×1.45     |
| 4 KB  | 32 KB      | 2.270 → 3.522         | ×1.55       | 2.282 → 3.611       | ×1.58     |
| 16 KB | 32 KB      | 2.810 → 3.722         | ×1.32       | 2.874 → 3.859       | ×1.34     |
| 32 KB | 32 KB      | 3.122 → 3.798         | ×1.22       | 3.219 → 3.958       | ×1.23     |

A dictionary helps WZIP_S about as much as it helps zstd. The remaining 2–4% difference in ratio is the
same as without a dictionary, so it comes from the block coding, not from the dictionary handling.
Decoding with a dictionary ran at the same speed as without on 4 KB blocks and about 16% slower on
16 KB blocks; zstd slowed by about 22% in the same test.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | the codecs: `WZIP.h` (WZIP_L, WZIP_M, WZIP_S), `WLZ4.h`, and their sources |
| `tests/roundtrip.c` | round-trip test of every codec and level (`make test`) |
| `bench/` | the benchmark harness and scripts of the paper (Windows, MSYS2); see `bench/README.md` |
| `results/` | the raw benchmark outputs behind the paper's tables |

## License

BSD 2-Clause (`LICENSE`). Parts of `src/WLZ4.c`, `src/WLZ4.h` and `src/Memry.h` derive from LZ4 and Zstandard;
their licenses are reproduced in `NOTICE`.
