# Benchmark harness

`bench_all.c` measures every codec of the paper the same way: each file is compressed whole by one library call,
on one thread pinned to one core at high priority, after a one-second warm-up. Ratio is total input over total
output; speeds are total input over total time, best of 3 compressions for fast codecs (1 otherwise) and best of 10
decompressions; every file is verified. Encoder memory is the growth of the peak working set during compression.
WZIP and WLZ4 decode in their opt-in trusted mode, as in the paper; LZ4 decodes with `LZ4_decompress_safe`.

```
bench_all <codec> <level> <compression rounds> <decompression rounds> file...
  codecs: wzip (0-13), zstd (1-22; negative: --fast), brotli (0-11, window 2^24), xz (0-9, add 100 for -e),
          lz4 (acceleration), lz4hc (1-12), wlz4f (acceleration), wlz4l (lazy), wlz4hc (0-12)
  env:    CHECKED=1        WZIP and WLZ4 decode with their default, bounds-checked decoders
          STREAMS=dir      decompression only, from streams saved in dir (a missing one is compressed and saved
                           first); STREAMS_ONLY=1 only prepares them, unpinned, so several can run in parallel
```

It uses Windows APIs (thread affinity, peak working set) and was built with MSYS2 MinGW-w64 and GCC 14.2:

```sh
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-brotli mingw-w64-x86_64-xz
make deps     # LZ4 1.10.0 and Zstandard 1.5.6 from source (the MSYS2 LZ4 package measured half as fast)
make          # bench_all.exe
```

`bench_blocks.c` measures independent fixed-size blocks instead (storage pages): each file is cut into blocks,
and each block is compressed and decompressed by its own call; `run_blocks.sh` reproduces the block tables of the
top-level README, including the length-2 experiment (`make S_W2=4 bench_blocks.exe` builds WZIP_S with length-2
matches within 16 bytes).

## Reproducing the paper

Corpora: Silesia (https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia), Canterbury and Calgary
(https://corpus.canterbury.ac.nz/), enwik8 and enwik9 (http://mattmahoney.net/dc/textdata.html).

```sh
SILESIA=... CANTERBURY=... CALGARY=... ENWIK=... ./run_paper.sh
```

`run_paper.sh` runs every configuration of `runlist.txt` on Silesia and on Canterbury+Calgary, a second pass of the
fast codecs (`runlist_fast.txt`), and the large inputs (`runlist.txt` on enwik8, `runlist_e9.txt` on enwik9); then
`gen_tables.py` writes the LaTeX tables, keeping the best speeds of repeated configurations, and `plots.py` the
Silesia trade-off figure. Timings need a quiet machine: AC power, a high-performance power plan, and no other load
(sync clients and chat apps disturbed ours by up to 15%).

WLZ4's format was revised in 2026-10 (flagged one- or two-byte offsets for lengths 4-5), after the other codecs
were measured. `run_wlz4.sh` (same variables) re-runs the runlists' LZ4-class lines, LZ4 and WLZ4 in one session,
into `results/lz4class_*.txt`, then (with `STREAMDIR` set) two more decompression-only passes from saved streams,
the configurations interleaved, and regenerates the tables and figure with those results in place of the earlier
ones of the same codecs (`gen_tables.py` SX/CX/E8X/E9X, `plots.py --replace`), each configuration keeping its best
speeds. `COOL=62` waits before each configuration until the CPU has cooled to 62 C, as in the paper's run: our
laptop otherwise slowed down by up to a third over a long session.

Zstandard 1.5.7 check: `make ZSTD=path/to/zstd-1.5.7/lib bench_all.exe`, then levels 19 and 22 as above.

Where WLZ4 sits between LZ4 and Zstandard (the top-level README's figure): `SILESIA=... COOL=62 STREAMDIR=...
./run_position.sh` measures LZ4, LZ4HC, WLZ4 and Zstandard's levels -7 to 9 (`runlist_position.txt`; negative
levels are `zstd --fast`) on Silesia in one session, then two decompression-only passes from saved streams, WLZ4 also
with its bounds-checked decoder, into `results/position_*.txt`; `plot_position.py` draws
`doc/figures/wlz4-position.svg` and prints the README's table.

WZIP ablation (the paper's "WZIP's parts"): `sh bench/ablation/run.sh path/to/silesia` from the repository root.
`ablation/switches.py` adds three encoder switches to a copy of `WZIP_L.c` in `build/` (never to `src/`):
`WZ_CLASSIC=1` codes every block with the 204-value alphabet, the cache slot going to the offset symbol (same parse);
`WZ_NG=k` uses k offset tables (`k=1`: one for all lengths; reparsed); `WZ_ONEWIN=1` gives lengths 3-7 the widest
window (reparsed; run it with `WZ_NG=5` and compare with `ng5`, since the decoder derives the groups from the windows).
`ablation/harness.c` compresses one file at level 11, verifies it with the checked decoder and prints the process's
peak working set.

WLZ4 ablation (the paper's Table 5 and its "offset size by length alone" sentence): `sh bench/ablation/wlz4/run.sh`
from the repository root (Windows, MSYS2; LZ4 from `make deps`, or `LZ4=path/to/lz4/lib`). It builds WLZ4 as of
2026-09-29, before the flagged offset: `WLZ4_twowin.c`, the two-window format (Table 5, row 2), and
`WLZ4_variants.c`, the same code with table-driven match codes, `-DWLZ_VARIANT=0` the far code (row 3) and `1`, `2`
offset size by length alone. Levels 10 and 12 run on Silesia and on Canterbury+Calgary. Row 4, the flagged format
before its 2026-10 revision, is `wlz4hc` of the main benchmark at commit 7c1e25b (`results/final_*.txt`); row 5, the
current format, is `wlz4hc` now (`results/lz4class_*.txt`). `wlz4/v3stat.c` counts the far-code parse's matches by length
and offset class.
