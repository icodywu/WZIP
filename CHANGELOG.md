# Changelog

Versions follow [semantic versioning](https://semver.org/) for the library interface. Format versions are separate:
a WZ frame records its frame format version and its codec's format version (`doc/frame_format.md`).

## Unreleased

- **Faster WZIP decoding of large inputs.** WZIP_L's decoders, checked and trusted, decode a block as a pipeline that
  prefetches each match's source when the previous block's matches often reached 1 MiB back or more: enwik8 44%
  and enwik9 62% faster at level 11 (AMD EPYC 9334), Silesia unchanged. Same format.
- **WZIP_L dictionaries searched fully.** The optimal levels (7-13) found only matches of 3-6 bytes in a dictionary
  (the nearest in each chain), never longer ones, which their binary tree finds in the input; they now insert the
  dictionary's positions within each window into both chains and the tree. At every level a match may now run from
  the dictionary into the input. Compressed with the 128 MiB before it as dictionary, a 128 MiB block of enwik9 now
  loses 0.19% against one stream at level 11 (3.6% before). Without a dictionary the output is unchanged. Streams
  with matches across the dictionary's end need the decoders of this version (the earlier trusted decoder copied
  them wrongly).
- **Fix:** WLZ4's encoders could read one byte past the input, after a literal run of 15 or 16 bytes before a match
  16 bytes from its end (found by fuzzing the round trip through 2 KB frame blocks).

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
