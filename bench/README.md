# Benchmark harness

`bench_all.c` measures every codec of the paper the same way: each file is compressed whole by one library call,
on one thread pinned to one core at high priority, after a one-second warm-up. Ratio is total input over total
output; speeds are total input over total time, best of 3 compressions for fast codecs (1 otherwise) and best of 10
decompressions; every file is verified. Encoder memory is the growth of the peak working set during compression.

```
bench_all <codec> <level> <compression rounds> <decompression rounds> file...
  codecs: wzip (0-13), zstd (1-22), brotli (0-11, window 2^24), xz (0-9, add 100 for -e), lz4 (acceleration),
          lz4hc (1-12), wlz4f (acceleration), wlz4l (lazy), wlz4hc (0-12)
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
