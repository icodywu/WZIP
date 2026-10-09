# WLZ4 and WZIP in lzbench

[lzbench](https://github.com/inikep/lzbench) is the in-memory benchmark most compression codecs are compared in.
`add_to_lzbench.sh` adds four entries to an lzbench checkout:

| lzbench name | Codec | Levels |
|---|---|---|
| `wlz4` | WLZ4, lazy mode | - |
| `wlz4fast` | WLZ4, fast mode (acceleration) | 1-99 |
| `wlz4hc` | WLZ4, hash chains (0-7) and optimal parsing (8-12) | 0-12 |
| `wzip` | WZIP (`wzip_compress_mt`: WZIP_L, or WZIP_M below 32 KB) | 0-13 |

They decompress with the default, bounds-checked decoders (`WLZ_Decompress`, `wzip_decompress`), as lzbench does
LZ4 (`LZ4_decompress_safe`), into a buffer of exactly the decoded size. `wzip` takes lzbench's internal threads
(`-I#`): at levels 7-13 its match finders run in up to 7 threads, with the output of one thread.

```sh
git clone https://github.com/inikep/lzbench && sh contrib/lzbench/add_to_lzbench.sh lzbench
cd lzbench && make -j8
./lzbench -ewlz4/wlz4hc,2,10,12/lz4/lz4hc,12/zstd,1,3,19/wzip,1,11 silesia.tar
./lzbench -ewzip,11 -I6 silesia.tar
```

The script copies the codec sources from `src/`, writes `mk/wlz4.mk` and `mk/wzip.mk` (WZIP is built with threads
unless lzbench is built with `DISABLE_THREADING=1`), and applies `lzbench.patch`: the benchmark functions
(`bench/lz_codecs.cpp`, `bench/lz_entropy_codecs.cpp`), their declarations (`bench/codecs.h`), the codec table and
aliases (`bench/lzbench.h`: LZ, LZ+ENTROPY, FASTEST, OPT), and the README and CHANGELOG entries that lzbench's
CONTRIBUTING.md asks for. The patch was made against lzbench commit `8bd060c` (2026-10-07); on a later lzbench it may
need small merges.

## Checked

What lzbench's CI runs (Azure Pipelines, about 30 configurations) was run on our four entries: the LZ and
LZ+ENTROPY aliases' levels on the lzbench binary, every level on incompressible and tiny inputs (256 KB, 40 KB, 100
and 5 random bytes, 1 byte, empty), FASTEST with `-T2 -jr` over the build tree, plus 64 KB chunks on 4 threads
(`-b64 -T4`) and `-I6`. All round trips verify, and the codec sources build without warnings, on:

- Linux x86-64: GCC 11.4 and clang (ROCm LLVM), and GCC 11.4 `-m32` (`BUILD_ARCH=32-bit`);
- Windows: MinGW-w64 GCC 14.2 (64-bit); the sources also compile with MSVC 2019;
- cross-built with the dockcross toolchains (`BUILD_STATIC=1`) and run under QEMU user mode: arm64, ARMv5
  (32-bit), ppc64le, and s390x (big-endian; lzbench's CI has no big-endian job).

These checks found that 32-bit and arm64 builds of WZIP were broken (see CHANGELOG); WZIP's own CI now builds and
tests with `gcc -m32`. Under QEMU (ARMv5) the LZ+ENTROPY alias's `wzip` levels take about 2.5 minutes on the lzbench
binary, the LZ alias's WLZ4 levels about 1. QEMU 7.2's arm64 emulation (Debian 12, in the dockcross images) itself
crashes about once in 300 runs under load, a trivial program as often; a lone arm64 failure in CI may be that.

## In lzbench

lzbench has them since [inikep/lzbench#341](https://github.com/inikep/lzbench/pull/341) (merged 2026-10-09 at
1.0.1, which holds the fixes from its review). On such an lzbench, `add_to_lzbench.sh` updates them instead: it
copies the sources and sets the version (from `src/WZIP.h`) and release date (from `CHANGELOG.md`) in the codec
table, the README line and the CHANGELOG entry. The repository must be public at that version's tag.

1. Update the fork's master from `inikep/lzbench`, branch, run `add_to_lzbench.sh` on it, build and run as above
   (lzbench's CI is described under Checked).
2. Commit as one change ("Update wlz4 and wzip to X.Y.Z"), push, and open a pull request against
   `inikep/lzbench`, saying what changed (CHANGELOG.md) and where the codecs stand (the top-level README has the
   numbers): WLZ4 decodes at 3.0-3.5 GB/s with its checked decoder, 1.6-2.4 times as fast as Zstandard at equal
   ratio, with ratios from 13% above LZ4's default mode to Zstandard 3's (16% above LZ4HC 12's), though Zstandard
   compresses faster at every ratio; WZIP 11-13 compress Silesia 3.5-4.1% more than Zstandard 22 (1.2% at level 11
   without the filters of 1.1.0) and decode at 83-88% of its speed in trusted mode (the checked decoder lzbench uses
   is 4-6% slower), and on enwik9 WZIP 13 passes xz -9e's ratio while decoding 3% slower than Zstandard 22; WZIP
   compresses more slowly than Zstandard 22.
