# Changelog

Versions follow [semantic versioning](https://semver.org/) for the library interface. Format versions are separate:
a WZ frame records its frame format version and its codec's format version (`doc/frame_format.md`).

## Unreleased

- **Threads in compression.** At WZIP's optimal levels (7-13) the three match-finder indexes (chain of lengths 3-4,
  chain of lengths 5-6, tree of lengths 7+) run in threads of their own beside the parser, and from 5 threads on the
  tree, a tree per hash bucket, splits among 2 to 4 threads by bucket (`WZIP_WORKERS_MAX`, 7): `wzip -T#`,
  `wzip_compress_mt`, `WZIP_Set_Workers`, `WZF_params.nbWorkers`. The output is byte for byte that of one thread;
  level 11 compresses Silesia 2.80 times as fast with 6 threads, enwik8 3.42 times with 7 (AMD EPYC 9334, one 8-core
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
  the output, which corrupted the heap or the next block. Outputs of 64-bit builds are unchanged;
  32-bit builds, whose match finders compare 4 bytes at a time, may write other (valid) streams. Found while adding
  the codecs to lzbench, whose CI builds them for 32-bit x86 and ARM; the CI here now builds and tests with
  `gcc -m32`.

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
