# WZIP and WLZ4

[![CI](https://github.com/icodywu/WZIP/actions/workflows/ci.yml/badge.svg)](https://github.com/icodywu/WZIP/actions/workflows/ci.yml)

Two LZ77 codecs built on **match-length-dependent sliding windows**: each match length has its own maximum
distance, so short matches use short, cheap offsets while long matches reach the whole history. Lengths are decoded
before offsets, so the decoder knows each match's window and no extra field is sent.

- **WZIP** is an entropy-coded, Zstandard-class codec (Huffman coding only), in three formats, because what pays on a
  large input costs too much on a small one ([comparison](#the-three-wzip-variants)):
  - **WZIP_L**, for inputs of 32 KB and more, sizes its windows to the input (up to 128 MB) and sends fresh Huffman
    tables, among them a 1020-symbol joint code, every 16,384 sequences, or reuses the last ones where they cost
    fewer bits: rich statistics whose description pays off on a large input.
  - **WZIP_M**, below 32 KB, sends one set of small tables, fixes its windows (8 KB for length 3, 32 KB beyond) so
    that no window header is needed, and splits its sequences into two streams that the decoder reads in parallel.
  - **WZIP_S**, for independent 4-8 KB storage pages, spends one header byte per page, reuses its context and a
    prepared dictionary across pages instead of rebuilding them, and adds features for short blocks: repeat offsets
    preset to 1, 4 and 8, length-2 matches at the last offset, and a two-byte form for a page of one repeated byte.

  `wzip_compress` picks WZIP_L or WZIP_M from the input size, which the stream records; WZIP_S has its own interface.
- **WLZ4** is a byte-aligned, LZ4-class codec with one offset rule: a one-byte offset for length 3, and for longer
  matches a flagged offset of 1 + (length >= 6) bytes, or one more when the flag is set: one or two bytes (128 B or
  32 KB) for lengths 4-5, two or three (32 KB or 8 MB) from length 6. It sits between LZ4 and Zstandard, nearer
  LZ4: on Silesia it reaches Zstandard 3's ratio and decodes 2.5 times as fast, at 68-78% of LZ4HC's speed, but compresses
  more slowly than Zstandard ([where WLZ4 sits](#where-wlz4-sits-between-lz4-and-zstandard)).

<img src="doc/figures/wlz4-windows.svg" alt="How far back a WLZ4 match can reach, by length and offset bytes" width="560">

*How far back a WLZ4 match can reach, by length and offset-field size, against LZ4's single 64 KB window
([more figures](doc/WLZ4_format.md)).*

**The decoded size comes first.** Every WZIP and WLZ4 stream begins with its decoded size: 2 bytes below 32 KB, else
4 (WZIP_S: two bits of its header byte for 4, 8 or 16 KB pages). A decoder therefore allocates its output exactly,
with no size kept beside the data (the LZ4 block format records none, LZ4 and Zstandard frames only optionally), and
checks that decoding ends exactly there. WZIP gets more from it: which format follows (a size of 0 marks stored
input), the window of WZIP_L's longest matches, which just covers the input, and the end of the stream: the last sequence
is the one whose literals reach that size, so no end-of-block symbol or sequence count is coded. Incompressible input
grows by at most 2 bytes with WZIP and 15 with WLZ4.

<img src="doc/figures/wzip-size.svg" alt="The WZIP size field and what a decoder derives from it" width="620">

*The size field of a `wzip_compress` stream, and what a decoder derives from it before reading the payload
([format](doc/WZIP_format.md#41-the-one-call-stream)).*

**Dictionary compression, simpler than zstd's.** A dictionary is just the history before the input, at negative
positions: one address space, one sign test to locate a match's source, and one chain walk over dictionary and input
together. WZIP_S indexes a dictionary once and shares it read-only across blocks, contexts and threads, never copying
or re-indexing it. See [Dictionary compression](#dictionary-compression).

## Results

Single thread on AMD EPYC 9334 nodes (Zen 4, clock capped at 2.7 GHz, 32 MB of L3 per 8-core complex), Ubuntu
22.04; every codec built from source with GCC 14.2 `-O2`, Zstandard with the assembly Huffman decoder its Linux builds
use. Each file is compressed whole. Each run is pinned to a core complex of its own, and every configuration ran
twice, on identical nodes; speeds are the best of both runs (of 3 compressions for fast codecs, 1 otherwise, and of
20 decompressions in each). Ratio is total input over total output; speeds in MB/s. Full tables, the harness and the
raw outputs are in [`bench/`](bench) and [`results/`](results) (`epyc_*`); [`bench/cluster/`](bench/cluster) runs
them on a Slurm cluster. A laptop (Intel Core i7-8850H, 9 MB L3, Windows 11), measured earlier (the other files in
`results/`), gave the same ratios at lower speeds. WZIP and WLZ4 decode in their trusted mode here, as in the paper
(see Usage); their default, bounds-checked decoders are 5-7% (WZIP) and at most 9% (WLZ4) slower on Silesia.

| Silesia (212 MB, 12 files) | Ratio | Compress | Decompress | Memory (MB) |
|---|---:|---:|---:|---:|
| LZ4HC 12 | 2.743 | 13.4 | 4520 | 0.9 |
| **WLZ4 6** | 2.820 | 40.1 | 3334 | 5.2 |
| **WLZ4 12** | 3.186 | 1.71 | 3535 | 41 |
| Zstandard 19 | 4.005 | 3.52 | 1393 | 82 |
| Zstandard 22 | 4.045 | 2.53 | 1312 | 642 |
| **WZIP 11** | 4.078 | 1.71 | 1194 | 566 |
| **WZIP 13** | 4.097 | 0.78 | 1138 | 949 |
| Brotli 11 | 4.276 | 0.68 | 506 | 241 |
| xz -9e | 4.374 | 2.41 | 149 | 505 |

| enwik9 (1 GB of Wikipedia) | Ratio | Compress | Decompress | Memory (MB) |
|---|---:|---:|---:|---:|
| Zstandard 22 | 4.676 | 1.41 | 966 | 649 |
| xz -9e | 4.722 | 1.43 | 165 | 674 |
| **WZIP 11** | 4.506 | 1.23 | 1115 | 550 |
| **WZIP 12** | 4.645 | 0.90 | 1029 | 1062 |
| **WZIP 13** | 4.759 | 0.47 | 950 | 1911 |

Memory is the encoder's (the growth of the peak resident set while compressing). **Each level has its own window**,
which sizes the encoder's tables: WZIP searches 2^27 bytes back at the top level of each parser (lazy 6, optimal 13)
and half as far per level below (2^21 at levels 0 and 7), WLZ4's hash-chain and optimal levels the format's 8 MiB at
levels 7 and 12, down to 64 KiB at level 0 and 512 KiB at level 8. Inputs no larger than a level's window compress
as at the top level; larger ones trade ratio for memory and speed, as enwik9 shows. A level above the top of the
lazy parser can then compress large inputs less than it (WZIP 7-8 below 6, WLZ4 8 below 7). Streams keep the
windows of their size, so decoders need no level.

### Where WLZ4 sits between LZ4 and Zstandard

<img src="doc/figures/wlz4-position.svg" alt="Silesia: compression ratio against decompression speed and against compression speed for LZ4, WLZ4 and Zstandard levels -7 to 9" width="760">

*Silesia, LZ4, WLZ4 and Zstandard, from the runs of the tables above (drawn by `bench/plot_position.py`). Labels are
levels, with f and l for WLZ4's fast and lazy modes; Zstandard's negative levels are its `--fast` modes. Hollow
squares: WLZ4's default, bounds-checked decoder. Arrows join configurations of equal ratio, labeled with the speed
ratio.*

In ratio and decompression speed WLZ4 lies between LZ4 and Zstandard; in compression speed it does not.

- **Decompression.** WLZ4 decodes at 3.1-3.5 GB/s in trusted mode, 68-78% of LZ4HC 12's 4.5 GB/s, and at
  3.0-3.5 GB/s with its default, bounds-checked decoder (LZ4 and Zstandard are measured with their checked
  decoders); Zstandard's levels -7 to 9 decode at 1.4-2.5 GB/s. At equal ratio WLZ4 decodes 1.6-2.5 times as fast
  as Zstandard (1.6-2.4 with the checked decoder): the lazy mode against Zstandard -1 (ratio 2.43), level 12 against
  Zstandard 3 (3.19).
- **Ratio.** WLZ4's levels span 2.37-3.19, from 13% above LZ4's default mode to Zstandard 3's ratio, 16% above
  LZ4HC 12's. Zstandard's higher levels go further (3.57 at level 9, 4.05 at 22), as does WZIP.
- **Compression speed.** At a given ratio WLZ4 compresses more slowly than Zstandard. Its fast and lazy modes run at
  307-328 MB/s, against 735 MB/s for LZ4 and 591 MB/s for Zstandard -1, which matches the lazy mode's ratio;
  Zstandard 3 reaches level 12's ratio 184 times as fast. Against LZ4HC, WLZ4 compares well: level 6 exceeds
  LZ4HC 12's ratio at 3.0 times its compression speed.

WLZ4 therefore suits data compressed once and decompressed many times (read-mostly storage, software packages,
game and web assets), where decoding near LZ4's speed matters more than encoding speed. For data compressed on the
fly, LZ4 and Zstandard's fast levels are the better choice.

<details><summary>All points of the figure</summary>

| Codec, level | Ratio | Compress | Decompress (checked) |
|---|---:|---:|---:|
| LZ4 1 | 2.101 | 735 | 4718 |
| LZ4HC 4 | 2.656 | 95.6 | 4276 |
| LZ4HC 9 | 2.721 | 38.8 | 4398 |
| LZ4HC 12 | 2.743 | 13.4 | 4520 |
| **WLZ4 fast** | 2.373 | 328 | 3061 (3007) |
| **WLZ4 lazy** | 2.432 | 307 | 3246 (3207) |
| **WLZ4 2** | 2.614 | 70.6 | 3483 (3386) |
| **WLZ4 4** | 2.728 | 55.0 | 3539 (3451) |
| **WLZ4 6** | 2.820 | 40.1 | 3334 (3429) |
| **WLZ4 8** | 2.812 | 20.4 | 3333 (3254) |
| **WLZ4 10** | 3.009 | 9.21 | 3424 (3128) |
| **WLZ4 12** | 3.186 | 1.71 | 3535 (3448) |
| Zstandard -7 | 1.937 | 791 | 2469 |
| Zstandard -5 | 2.056 | 728 | 2337 |
| Zstandard -3 | 2.239 | 664 | 2175 |
| Zstandard -1 | 2.437 | 591 | 2034 |
| Zstandard 1 | 2.887 | 521 | 1629 |
| Zstandard 3 | 3.186 | 315 | 1438 |
| Zstandard 6 | 3.444 | 117 | 1509 |
| Zstandard 9 | 3.570 | 78.4 | 1571 |

</details>

### Independent 4 KB and 8 KB blocks

Storage pages and key-value stores compress small blocks independently. Here every Silesia file is cut into 4 KB
or 8 KB blocks, each compressed and decompressed by its own call (`bench/bench_blocks.c`, on the EPYC as above, best
of two runs; results for Canterbury+Calgary are in [`results/epyc_blocks_C.txt`](results/epyc_blocks_C.txt)).

| Silesia in blocks | 4 KB ratio | Compress | Decompress | 8 KB ratio | Compress | Decompress |
|---|---:|---:|---:|---:|---:|---:|
| LZ4 | 1.727 | 742 | 3514 | 1.824 | 731 | 3770 |
| LZ4HC 12 | 1.855 | 49.1 | 3846 | 2.003 | 40.4 | 4237 |
| **WLZ4 2** | 1.901 | 85.5 | 2822 | 2.027 | 87.1 | 3052 |
| **WLZ4 10** | 1.945 | 33.8 | 2640 | 2.087 | 31.0 | 2814 |
| Zstandard 1 | 2.315 | 296 | 811 | 2.476 | 344 | 1001 |
| Zstandard 9 | 2.424 | 50.6 | 813 | 2.619 | 47.6 | 980 |
| Zstandard 19 | 2.522 | 6.55 | 700 | 2.736 | 5.85 | 821 |
| **WZIP_S 1** | 2.434 | 77.8 | 455 | 2.580 | 85.8 | 574 |
| **WZIP_S 5** | 2.499 | 48.5 | 473 | 2.680 | 46.4 | 612 |
| **WZIP_S 9** | 2.500 | 38.0 | 475 | 2.682 | 29.8 | 613 |
| **WZIP_M 12** | 2.503 | 1.34 | 572 | 2.718 | 0.84 | 704 |

WZIP_S 1 compresses 5% more than Zstandard 1 on 4 KB blocks, and WZIP_S 5 3% more than Zstandard 9 at about the
same compression speed, but WZIP_S decodes at 56-59% of the speed of Zstandard 1 and 9, and Zstandard 19 still
compresses 1-2% more than WZIP_S 9. WLZ4 compresses 1-5% more than LZ4HC 12, decoding 27-34% slower (13-22% on the
laptop, `results/blocks_*.txt`). WZIP_M, reached through `wzip_compress`, compresses slowly on small blocks because
the one-call interface builds its tables on every call; WZIP_S keeps them in a reusable context.

## Papers

- Y. Wu, "WZIP and WLZ4: LZ77 codecs with match-length-dependent sliding windows," submitted to the 2027
  Data Compression Conference ([preprint PDF](papers/WZIP_WLZ4_DCC.pdf)). Describes both codecs, their optimal
  parser and the measurements above.
- Y. Wu, "Improved LZ77 compression with match-length-dependent sliding windows," submitted to IEEE Transactions on
  Information Theory, [arXiv:2610.06530](https://arxiv.org/abs/2610.06530) ([PDF](papers/WLZ.pdf)). Analyzes the
  scheme: universality of greedy parsing, schedule calibration, run and period gains, and exact minimum-cost parsing.
- Y. Wu, "Improved LZ77 compression," in Proc. Data Compression Conference, 2021, p. 377. Introduces the scheme.

## Format specifications

The papers measure the codecs; the formats themselves are specified in [`doc/`](doc), precisely enough to write
an independent decoder, together with the reference decoders' techniques and the encoders' levels, and illustrated
with figures of every layout:

- [`doc/WLZ4_format.md`](doc/WLZ4_format.md): the WLZ4 block, with a byte-by-byte worked example;
- [`doc/WZIP_format.md`](doc/WZIP_format.md): the `wzip_compress` stream (WZIP_L, WZIP_M) and WZIP_S blocks;
- [`doc/frame_format.md`](doc/frame_format.md): the WZ frame, the container for files: magic number, codec and
  format version, blocks of bounded size, content size and XXH32 checksum.

Each comes with a small decoder written from the specification alone ([`doc/wlz4_decode.py`](doc/wlz4_decode.py),
[`doc/wzip_decode.py`](doc/wzip_decode.py), [`doc/frame_decode.py`](doc/frame_decode.py)), which checks every
rule and decodes the output of every encoder level identically; they are slow, and meant as executable references.

## Command-line tool

`wzip` compresses with WZIP, and the same program called `wlz4` with WLZ4; either decompresses both. Files are
written as WZ frames, with a checksum, and processed block by block, so pipes and files of any size work.

```sh
wzip file              # file.wz (level 1; -0 to -13, 7-13 optimal parsing)
wlz4 -10 file          # file.wlz4 (lazy mode by default; --fast, -0 to -12)
wzip -d file.wz        # back to file (-o name, -c to standard output)
tar cf - dir | wlz4 > dir.tar.wlz4
wzip -t file.wz        # test; -l lists frames, blocks, codec, sizes and ratio
wzip -b9 -e11 file     # benchmark levels 9 to 11 in memory
wzip -11 -T6 file      # 6 threads: 2.2-3.2 times as fast at levels 7-13, the same output
wzip -11 -T64 big      # more threads: blocks compressed at once (below); wzip -d needs no option
```

As in Zstandard's tool, inputs are kept unless `--rm` is given, and outputs are not overwritten without `-f`.
`wzip -h` lists every option.

**Threads.** At its optimal levels (7-13) WZIP spends most of its time finding matches, in three indexes, one per
class of lengths, each with its own window: a 3-byte chain for lengths 3-4, a 5-byte chain for lengths 5-6 and a
binary tree for lengths 7 and up. They depend only on the input, not on the parse, so with `-T` each runs in a thread
of its own, ahead of the parser, which takes their candidates in order; the tree, the slowest, is one tree per hash
bucket, and splits further among up to four threads by bucket. The input is not cut, and the output is byte for byte
that of one thread. On AMD EPYC 9334 (the threads on one 8-core complex), level 11 compresses Silesia 2.26 times as
fast with 4 threads and 2.83 times with 6, and enwik8 2.34 and 3.57 times with 4 and 6; level 7 Silesia 2.24 times,
level 13 2.32 times. With 5 threads or more the parser is the slowest stage.

More threads than one block uses (at levels 0-6, more than one) go to blocks: content over 64 MiB, or from a pipe,
is cut into blocks of 64 MiB, compressed at once, each **linked** to the content before it within the level's window,
its dictionary, which its threads index first ([frame format](doc/frame_format.md), section 5.1; at level 0, which
searches 1 MiB, the blocks are independent). One thread keeps one stream, at full speed; the blocks lose almost
nothing to the cuts. Whoever decompresses needs no option: the frame says how it was made. On a node of two
EPYC 9334, threads unpinned:

| enwik9 | 1 thread | 8 threads | 16 threads | 105 threads | ratio, one stream / blocks |
|---|---|---|---|---|---|
| level 1 | 123 MB/s | 732 MB/s | 1473 MB/s | | 3.3444 / 3.3434 |
| level 5 | 8.44 MB/s | 41.4 MB/s | 64.3 MB/s | | 4.0927 / 4.0926 |
| level 11 | 1.22 MB/s | 7.26 MB/s | 11.0 MB/s | 21.7 MB/s | 4.5059 / 4.5056 |
| level 13 | 0.47 MB/s | | | 6.40 MB/s | 4.7593 / 4.7586 |

(Level 13 with one thread: the benchmark above.) Linked blocks decode in one thread, 3-5% slower than one stream,
with the level's window and a block in memory (`results/epyc_linked.txt`).

## Python

```sh
pip install .            # from this repository (a C compiler is needed); the package is wzip
```

```python
import wzip
packed = wzip.compress(data, "wlz4", level=10)   # or "wzip", levels 0-13 (default 1)
assert wzip.decompress(packed) == data           # the tools' WZ frames, either codec; wzip.Error on damage
```

Calls release the GIL, so threads compress in parallel; [`python/README.md`](python/README.md) has the rest.
`.github/workflows/wheels.yml` builds wheels for Linux, macOS and Windows, for publication on PyPI.

## Build and test

```sh
make          # build/libwzip.a, the tool build/wzip (and build/wlz4), and the tests; with threads (make MT=0: none)
make test     # round trips of every codec and level on synthetic inputs, with guard checks on every buffer, also
              # with windows narrowed to 128 KiB (a test format) so that they wrap the encoders' indexes;
              # 8 threads at once against single-threaded outputs; the tool on files, pipes and damaged files;
              # the golden frames of tests/golden, which every version must keep decoding
make check FILES="file1 file2"
make fuzz     # damaged streams of every codec and of frames, under AddressSanitizer and UndefinedBehaviorSanitizer
CC=clang sh tests/fuzz/run.sh /tmp/fuzz 600 4   # coverage-guided fuzzing (libFuzzer) of every decoder and round trip
```

CMake builds the static and shared libraries, the tools and the tests, and installs them with a pkg-config file:

```sh
cmake -S . -B build-cmake -DCMAKE_INSTALL_PREFIX=/usr/local && cmake --build build-cmake
ctest --test-dir build-cmake && cmake --install build-cmake
```

The sources (`src/`) are C99 and build without warnings (`-Wall`) with GCC 14.2 (MinGW-w64, Windows), and GCC 11.4
and clang 14 (Ubuntu 22.04, x86-64), where the tests also pass under AddressSanitizer and UndefinedBehaviorSanitizer,
and the thread test under ThreadSanitizer. Continuous integration ([`.github/workflows/ci.yml`](.github/workflows/ci.yml))
runs these on Linux (x86-64 and arm64, GCC and clang), macOS (arm64) and Windows (MinGW-w64 and MSVC), and builds
with CMake.
Big-endian support is not yet verified: a job on an emulated s390x runs the tests for information only. On x86 the
decoders pick BMI2 code paths at run time. The library's version
(1.0.0) is in `WZIP.h`; [`CHANGELOG.md`](CHANGELOG.md) lists the changes.

The default decoders validate their input, as LZ4's safe decoder does: whatever the stream, a decoder reads nothing
outside the compressed buffer (and the dictionary) and writes nothing outside the output buffer; a corrupt or truncated
stream returns 0. `make fuzz` decodes flipped, overwritten, truncated, spliced and extended streams of every codec from
buffers of exactly their size, so that the sanitizers catch any stray access; it also decodes each undamaged stream
from such a buffer, and in trusted mode from a buffer with exactly the documented slack.

## Usage

For files, or whenever the data must identify itself, use the WZ frame (`wzframe.h`): it records the codec and its
format version, splits content of any size into blocks, and checks an XXH32 checksum. Its functions take `size_t`
sizes and return error codes.

```c
#include "wzframe.h"

WZF_params p = { WZF_CODEC_WLZ4, 10, 0, 0 };          /* codec, level, block size log (0: auto), noChecksum */
size_t cap = WZF_compressBound(n, &p);
size_t cSize = WZF_compress(dst, cap, src, n, &p);    /* check WZF_isError(cSize) */

unsigned long long size = WZF_getContentSize(dst, cSize);
size_t dSize = WZF_decompress(out, size, dst, cSize); /* the content size, or an error: WZF_getErrorName(dSize) */
```

`WZF_compressBegin`/`Block`/`End` and `WZF_decompressBegin`/`nextBlock`/`Block`/`End` do the same block by block,
for content that does not fit in memory. The codecs' own one-call functions below produce bare streams, without
identification or checksum, for applications that store sizes and codecs themselves:

```c
#include "WZIP.h"

int cap = WZIP_Cap_CmprSize(n);                       /* input + 256 always suffices */
int cSize = wzip_compress(src, n, dst, &cap, 11);     /* level 0-13; 0 on failure */
/* or wzip_compress_mt(src, n, dst, &cap, 11, 4): 4 threads, the same stream (WZF_params.nbWorkers for frames) */

int decCap = n;                                       /* the decoded size, also given by WZIP_Read_DecSize */
int dSize = wzip_decompress(dst, cSize, out, &decCap);
```

```c
#include "WLZ4.h"

WLZhc_State_Str* hc = WLZhc_New_State();
unsigned cSize = WLZhc_Compress(hc, src, dst, n, WLZ_COMPRESSBOUND(n), 10);   /* level 0-12 */
unsigned dSize = WLZ_Decompress(dst, out, cSize, n);                         /* out holds n bytes */
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

On Silesia the trusted mode decodes 5-7% faster for WZIP and up to 9% faster for WLZ4 on the EPYC (9-15% for WLZ4
on the laptop). Never use it on data that
may be damaged or crafted: a bad stream can make it read or write out of bounds.

Every function is thread-safe when each thread uses its own contexts (`make test` runs several threads at once);
a prepared WZIP_S dictionary may be shared.

## Limitations

- WZIP's encoder memory follows the level's window: on a 50 MB input about 570 MB at level 11 and 940-950 MB at
  12-13, on enwik9 0.55, 1.06 and 1.9 GB at levels 11, 12 and 13 (Zstandard 22: 0.65 GB). The ratio of a large input drops
  with the window (enwik9: 4.759, 4.645, 4.506 at levels 13, 12, 11). A codec stream holds at most 2 GB, and windows
  reach 128 MB; the WZ frame stores larger content as several blocks.
- A WZIP_L literal run holds at most 2^24 - 1 bytes: an input with about 16 MB in which no match is found (and data
  after it worth compressing) is stored rather than compressed.

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

zstd addresses dictionary positions through a base pointer and limits, keeps an attached dictionary's tables apart
from the input's and searches them separately, and copies a prepared dictionary into the context for larger inputs
(details in the comparison below). Treating the dictionary as ordinary history removes that bookkeeping:

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
On the laptop, decoding with a dictionary ran at the same speed as without on 4 KB blocks and about 16%
slower on 16 KB blocks; zstd slowed by about 22% in the same test.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | the codecs: `WZIP.h` (WZIP_L, WZIP_M, WZIP_S), `WLZ4.h`, the frame `wzframe.h`, and their sources |
| `doc/` | format specifications (WZIP, WLZ4, the WZ frame) and reference decoders (Python) |
| `programs/wzip.c` | the command-line tool `wzip` / `wlz4` |
| `tests/` | round trips of every codec and level, the thread test, the tool's test (`make test`), damaged streams (`make fuzz`) |
| `bench/` | the benchmark harness and scripts of the paper (Windows with MSYS2, or Linux; `bench/cluster/` for Slurm clusters); see `bench/README.md` |
| `results/` | the raw benchmark outputs behind the paper's tables |
| `contrib/lzbench/` | adds WLZ4 and WZIP to [lzbench](https://github.com/inikep/lzbench) |
| `contrib/turbobench/` | adds WLZ4 and WZIP to [TurboBench](https://github.com/powturbo/TurboBench) |
| `python/` | the Python package `wzip` (`pyproject.toml`, `setup.py` at the root) |

## License

BSD 2-Clause (`LICENSE`). Parts of `src/WLZ4.c`, `src/WLZ4.h` and `src/Memry.h` derive from LZ4 and Zstandard;
their licenses are reproduced in `NOTICE`.
