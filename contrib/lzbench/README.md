# WLZ4 and WZIP in lzbench

[lzbench](https://github.com/inikep/lzbench) is the in-memory benchmark most compression codecs are compared in.
`add_to_lzbench.sh` adds four entries to an lzbench checkout:

| lzbench name | Codec | Levels |
|---|---|---|
| `wlz4` | WLZ4, lazy mode | - |
| `wlz4fast` | WLZ4, fast mode (acceleration) | 1-99 |
| `wlz4hc` | WLZ4, hash chains (0-7) and optimal parsing (8-12) | 0-12 |
| `wzip` | WZIP (`wzip_compress`: WZIP_L, or WZIP_M below 32 KB) | 0-13 |

They decompress with the default, bounds-checked decoders (`WLZ_Decompress`, `wzip_decompress`), as lzbench does
LZ4 (`LZ4_decompress_safe`), into a buffer of exactly the decoded size.

```sh
git clone https://github.com/inikep/lzbench && sh contrib/lzbench/add_to_lzbench.sh lzbench
cd lzbench && make -j8
./lzbench -ewlz4/wlz4hc,2,10,12/lz4/lz4hc,12/zstd,1,3,19/wzip,1,11 silesia.tar
```

The script copies the codec sources from `src/`, writes `mk/wlz4.mk` and `mk/wzip.mk`, and applies
`lzbench.patch`: the benchmark functions (`bench/lz_codecs.cpp`, `bench/lz_entropy_codecs.cpp`), their declarations
(`bench/codecs.h`), the codec table and aliases (`bench/lzbench.h`: LZ, LZ+ENTROPY, FASTEST, OPT), and the README and
CHANGELOG entries that lzbench's CONTRIBUTING.md asks for. The patch was made against lzbench commit `1db5b9b`
(2026-10-02); on a later lzbench it may need small merges.

Checked with MinGW-w64 GCC 14.2: lzbench builds without warnings in these files, lists the four codecs (`-l`), and
verifies every round trip of the levels above, whole files and 64 KB chunks on 4 threads (`-b64 -T4`). The sources also
compile with MSVC 2019.

## Proposing them to lzbench

lzbench's CONTRIBUTING.md asks that a new codec be significant in some dimension and pass its CI, which builds on about
30 configurations: GCC and clang versions, MSVC, macOS, 32- and 64-bit ARM, **big-endian PowerPC** and RISC-V. Our own
CI tests big-endian only for information so far (`.github/workflows/ci.yml`), so check that job before opening the
pull request: a failure there would fail lzbench's CI too.

1. Fork lzbench on GitHub, clone the fork, run `add_to_lzbench.sh` on it, build and run as above.
2. Commit as one change ("Add wlz4 1.0.0 and wzip 1.0.0"), push, and open a pull request against
   `inikep/lzbench`, saying what each entry is for: WLZ4 decodes at 71-83% of LZ4's speed with ratios up to Zstandard
   3's; WZIP compresses Silesia 0.9-1.3% more than Zstandard 22 (see the top-level README).
