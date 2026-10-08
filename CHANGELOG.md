# Changelog

Versions follow [semantic versioning](https://semver.org/) for the library interface. Format versions are separate:
a WZ frame records its frame format version and its codec's format version (`doc/frame_format.md`).

## Unreleased

- **A window per level.** Each level searches at most its own window, so that memory follows the level: WZIP
  2^27 bytes at the top level of each parser (6 and 13) and one bit less per level below it (2^21 at levels 0 and
  7; `WZIP_LEVEL_WINDOW_LOG`), WLZ4's hash-chain and optimal levels the format's 2^23 at levels 7 and 12, down to 2^16
  at level 0 and 2^19 at level 8 (`WLZhc_LEVEL_WINDOW_LOG`). WZIP's cap applies to the widest of the windows the
  input size sets, and each narrower window moves down just enough to stay below the next wider one, so that the
  windows keep their order. Only the encoders change: streams keep the windows of their size, decoders need no level,
  and an input no larger than a level's window compresses exactly as before. On Silesia WZIP 11 needs 566 MB instead
  of 948 MB (Zstandard 22: 642 MB) at 0.04% lower ratio; on enwik9, where the windows bind, levels 11 and 12 reach
  4.505 and 4.644 (11 reached 4.744) and level 13 keeps 4.759. The levels just above the top of the lazy parser now compress large inputs less than
  it (WZIP 7-8 below 6, WLZ4 8 below 7). Linked blocks in WZ frames take the level's window.
- **No buffer of the input's size for WZIP's sequences.** WZIP_L's encoders coded the blocks of sequences into a
  buffer as large as the input and copied them behind the literals at the end. Each block is now coded into a
  scratch buffer of one block and stacked at the end of the output buffer, which the literals fill from the start;
  at the end the blocks move up behind the literals. The output is byte for byte the same, and the input is stored
  as before when literals and sequences together exceed the output's capacity. On enwik9 level 0 needs 1.3 MB
  instead of 192 MB, level 3 129 MB instead of 357 MB, level 11 550 MB instead of 745 MB and level 13 1.9 GB instead
  of 2.1 GB; on Silesia level 0 1.5 MB instead of 15 MB, levels 11-13 about the same (their tables dominate).
- **Reused Huffman codes** (WZIP_L format; the format had not been released, so its codec format version stays 1 and
  streams of the earlier form no longer decode: the golden frames of WZIP_L were written again). A literal block of
  type 2 reuses the code of the stream's last block of type 1, and a sequence block may reuse each of its codes
  (literal run, joint symbol, each offset group) from the last block that sent it: one bit per code after the
  block's first bit (`doc/WZIP_format.md`, 4.2 and 5.4). The encoder reuses a code where the block's symbols take
  fewer bits with it than with a code of their own plus its lengths, as on a short last block; the decoder then reads
  no lengths and keeps the table it built. Ratios rise by about 0.02% (Silesia 4.0787 at level 11, was 4.0783). The
  reference decoder (`doc/wzip_decode.py`) follows.
- **Two sequence streams** (WZIP_L format, as above: codec format version 1, golden frames written again). A
  sequence block's sequences alternate between two bit streams, A (sequences 0, 2, 4, ...) and B (1, 3, 5, ...),
  after a `u24` size of A (`doc/WZIP_format.md`, 5.4). A Huffman code starts where the previous one ends, so the
  codes of one stream decode one after another: three dependent table lookups a sequence, about 25-30 cycles on AMD
  EPYC 9334 (enwik8, sequences alone) against Zstandard's 15-16, whose FSE states make its three lookups
  independent. With two streams the decoder decodes two sequences at once (its two readers swap after each
  sequence): decoding alone falls to 18-22 cycles, and whole decoding gains 6-16% on Silesia, 14-25% on the small
  files (Canterbury and Calgary) and 0-24% on enwik8; on enwik9 13% at level 0, 6-7% at levels 3 and 9 and -2% to 0%
  at levels 11-13. The cost is 3 bytes and an alignment a block (ratios about 0.01% lower).
  (Splitting the fields instead, literal runs, lengths and offsets in three streams, had been slower: the offset
  still waits for the length, and each field needs its own reload.)
- **WZIP_S decodes 18-21% faster** (its streams are unchanged; the encoder change below is one any decoder
  accepts). Its literal-run, match-length and offset tables hold each symbol's whole value, the extra-bit count and
  the value's high bits beside the code length, so that a field takes one table lookup and one shift instead of a
  second lookup and a branch, as Zstandard's sequence tables do; the code lengths are read with a refill every few
  lengths rather than after each, counted and checked in one pass, and the tables filled up to four entries at a
  time. In blocks up to 8 KB the encoder limits literal codes to 9 bits (the format allows 10): 4 KB blocks
  compress 0.1% more, 8 KB blocks within 0.01%. Silesia in 4 KB blocks decodes at 544-572 MB/s instead of 455-475
  (67-70% of Zstandard 1 and 9 instead of 56-59%), in 8 KB blocks at 679-737 instead of 574-613 (AMD EPYC 9334). A
  profile of 4 KB blocks had found two thirds of the gap to Zstandard in the sequences (WZIP_S's parse makes 1.5
  times as many, its length-3 matches buying its ratio) and a fifth in building the literal table.
- **WZIP_S: four literal streams from 4 KB blocks, meeting at one point** (WZIP_S block format; it had not been
  released, so its format version stays 0 and blocks of the earlier form above 2 KB no longer decode). Blocks above
  2 KB split their literals into four streams: besides the main stream and stream B, streams C and D meet at a point
  P of the block, which the main stream records as the bytes from P to the block's end (`log2` of the block size
  bits): C is stored byte-reversed before P and read backward from it, D read forward from P, so that one point
  serves two streams, as the block's end serves stream B (`doc/WZIP_format.md`, 7.3). Before, four streams began
  at 8 KB blocks, with two 16-bit stream sizes. On Silesia (AMD EPYC 9334) 4 KB blocks decode 8-9% faster (589-623
  MB/s, 72-77% of Zstandard 1 and 9) for 0.14% less compression; 8 KB blocks compress 0.09% more (16 KB 0.05%, 32
  KB 0.02%) at the same speed. The reference decoder (`doc/wzip_decode.py`) and Figure 10 follow.
- **Literals decoded as needed.** WZIP_L's decoders decoded the whole literal stream into a buffer of its size (the
  checked decoder: of the output's size) before the first sequence. Up to 32 MiB of literals, which stay in the
  last-level cache, they still do (into a buffer of the literals' size); a stream with more is decoded a 32 KiB block
  at a time as the sequences reach it, into a buffer of 128 KiB (`LIT_EagerMax`, `Lit_Reader`): enwik9 at level 0
  decodes 7% faster. The checked decoder checks each literal run against the literals and rejects runs past
  them, as the specification asks (it used to read zeros); it decodes Silesia 2-5% faster. Literal decoding
  tables are built in place, not allocated per block.
- **Threads in compression.** At WZIP's optimal levels (7-13) the three match-finder indexes (chain of lengths 3-4,
  chain of lengths 5-6, tree of lengths 7+) run in threads of their own beside the parser, and from 5 threads on the
  tree, a tree per hash bucket, splits among 2 to 4 threads by bucket (`WZIP_WORKERS_MAX`, 7): `wzip -T#`,
  `wzip_compress_mt`, `WZIP_Set_Workers`, `WZF_params.nbWorkers`. The output is byte for byte that of one thread;
  level 11 compresses Silesia 2.83 times as fast with 6 threads, enwik8 3.57 times with 7 (AMD EPYC 9334, one 8-core
  complex). With a dictionary all the threads index it at once. When the history outgrows the tree's 2^27-byte
  window, the window now stops 32 KiB short, with any number of threads, so that the tree's threads never reuse a
  node another still reads: enwik9 at level 11 grows by 0.008%. Built by default with make and CMake
  (`WZIP_MULTITHREAD`; pthreads, or Win32 threads on Windows); `make MT=0` and `-DWZIP_MULTITHREAD=OFF` leave it out.
- **Linked blocks in WZ frames** (frame format: FLG bit 4 and a window log byte, `doc/frame_format.md`, 5.1): each
  WZIP block may refer to the up to 2^W bytes of content before it (W up to 27, WZIP's widest window) as its
  dictionary, so that content cut into blocks, and so compressed in parallel, loses almost nothing to the cuts:
  enwik9 in blocks of 64 MiB, each with the 128 MiB before it, compresses within 0.01% of one stream at level 11.
  They are written when there are more threads than one block uses (`-T2` and up at levels 0-6, `-T8` and up at
  levels 7-13; `WZF_params.nbWorkers` and the Python package's `threads` alike), for content over 64 MiB or of
  unknown size; independent at level 0. One thread writes one stream as before, so its speed is unchanged; each
  linked block indexes its dictionary first (at level 11 about half the cost of compressing it), which the threads
  pay for. `WZF_params.windowLog` asks for linked blocks of another window, or with -1 never; decompression needs no
  option or parameter, and decoders of 1.0.0 reject linked blocks.
- **Blocks compressed in parallel.** `WZF_compress` and the new `WZF_compressBlocks` (streaming, any number of
  blocks per call) compress `nbWorkers` blocks at once, of either codec, each with what is left of the threads for
  WZIP's match finder; for a given layout of blocks the frame is the same for any number of threads.
- With a dictionary, WZIP's levels 0-6 index each of their tables only as far back as it reaches (its window; for a
  table without a chain, 16 MiB), and levels 7-13 no longer build the tables of levels 0-6, which they do not use
  (about 1 GB less per linked block). Outputs without a dictionary are unchanged.
- `wzip_compress_usingDict` and `wzip_decompress_usingDict`: the one-call stream with a dictionary (WZIP_L).
- Python: `wzip.compress(..., threads=)`.
- **Faster WZIP decoding of large inputs.** WZIP_L's decoders, checked and trusted, decode a block as a pipeline that
  prefetches each match's source when the previous block's matches often reached 1 MiB back or more: enwik8 44%
  and enwik9 62% faster at level 11 (AMD EPYC 9334), Silesia unchanged. Same format.
- **WZIP_L dictionaries searched fully.** The optimal levels (7-13) found only matches of 3-6 bytes in a dictionary
  (the nearest in each chain), never longer ones, which their binary tree finds in the input; they now insert the
  dictionary's positions within each window into both chains and the tree. At every level a match may now run from
  the dictionary into the input. Compressed with the 128 MiB before it as dictionary, a 128 MiB block of enwik9 now
  loses 0.19% against one stream at level 11 (3.6% before). Without a dictionary the output is unchanged.
- **WZIP_L windows with a dictionary** follow the history, dictionary and input together (`doc/WZIP_format.md`,
  5.2): a small input can reach as far into a large dictionary as one stream reaches back. This changes the format
  of WZIP_L streams with a dictionary (made only through `WZIP_New_State_L` and `WZIP_Decompress_L`): those of
  earlier versions may not decode with this one, nor the reverse. Streams without one (those of `wzip_compress` and
  of WZ frames without linked blocks) are unchanged.
- **Fix: wrong data from WZIP's encoders on large inputs.** They started the offset cache with four equal
  placeholders, which a match could reach once the input outgrew WZIP's widest window (levels 0 and 1: 2^27 bytes) or
  1,061,109,567 bytes (levels 2-13), while the cache still held one: its offset then matched all of them at once and
  was written as a wrong offset, and the stream decoded without error to wrong data (a WZ frame's checksum catches
  it). Level 0 did so on 1 MiB of random bytes and 127 MiB of zeros, twice over; a test build with narrower windows
  (`WZIP_TEST_MAX_OFF_WIDTH`, now in `make test`) exposed it. The placeholders now lie above every offset; outputs
  that were right are unchanged.
- **Fix:** WLZ4's encoders could read one byte past the input, after a literal run of 15 or 16 bytes before a match
  16 bytes from its end (found by fuzzing the round trip through 2 KB frame blocks).
- **Fix: 32-bit and arm64 builds.** WZIP's bit streams took the width of their 64-bit container from the machine
  word, so that 32-bit builds wrote streams no decoder reads (and failed assertions); the container is now 64 bits on
  every target. WZIP's level 1 takes a match of whole words at once, which with 4-byte words may be shorter than the
  window it was found in allows; it now checks the window of its length. On arm64, `Memry.h` used NEON intrinsics
  without including `arm_neon.h`, which compilers reject or fail to link, and its wild copy ran in 32-byte steps,
  writing up to 31 bytes past its end where the decoders leave room for 15: the bounds-checked decoders wrote past
  the output, which corrupted the heap or the next block. Outputs of 64-bit builds are unchanged; 32-bit builds,
  whose match finders compare 4 bytes at a time, may write other (valid) streams. Found while adding the codecs to
  lzbench, whose CI builds them for 32-bit x86 and ARM; the CI here now builds and tests with `gcc -m32`.
- `contrib/turbobench`: adds WLZ4 and WZIP to TurboBench; `contrib/lzbench` follows lzbench of 2026-10-07 and gives
  `wzip` lzbench's internal threads (`-I#`).

## 1.0.0 (2026-10)

First release.

- **Formats.** WZIP format 1 and WLZ4 format 1 (`doc/WZIP_format.md`, `doc/WLZ4_format.md`). WLZ4's format 1 is
  the revision of October 2026 (flagged offsets for lengths 4-5); blocks written by earlier repository versions are
  not compatible and carry no version field. WZ frame format 0 (`doc/frame_format.md`): magic number, codec and
  format version, blocks of bounded size, content size and XXH32 checksum. Reference decoders in Python for all three.
- **Frame API** (`wzframe.h`): one-shot and block-by-block (streaming) compression and decompression, `size_t` sizes,
  error codes, content beyond the codecs' 2 GB limit as several blocks.
- **Command-line tool** `wzip` / `wlz4`: compress, decompress, test, list and benchmark, through files or pipes.
- **Python package** `wzip` (`pip install .`): WZ frames and the bare codec streams, releasing the GIL.
- WLZ4's bounds-checked decoder needs no room past the decoded size (it used to need 32 bytes).
- **lzbench integration** (`contrib/lzbench`).
- **Thread safety:** every function may run in several threads at once, each thread with its own contexts (WZIP_L's
  window schedule used to be global).
- **Builds:** GNU make (static library, tool, tests) and CMake (static and shared libraries, tool, tests, install,
  pkg-config).
- Validated decoders by default, with an opt-in trusted mode; WZIP_S dictionaries prepared once and shared.
