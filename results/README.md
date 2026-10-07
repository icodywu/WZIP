# Raw results of the paper

Output of `bench/bench_all` (one line per codec and level: ratio, compression and decompression speed in MB/s,
encoder memory in MB, verification failures) and of `bench/bench_blocks`. WZIP and WLZ4 were measured with the
decoders that are now their opt-in trusted mode (`wzip_decompress_trusted`, `WLZ_Decompress_Trusted`), the same code,
unless a file name says `checked`.

## AMD EPYC 9334 (the paper's and the README's numbers)

Nodes of two AMD EPYC 9334, Ubuntu 22.04, GCC 14.2, run by `bench/cluster/` (see `bench/README.md`), one run per
8-core complex. The paper's tables are `gen_tables.py` applied to `epyc_*` and `epyc_control_*` together:

| File | Contents |
|---|---|
| `epyc_S.txt`, `epyc_C.txt`, `epyc_e8.txt`, `epyc_e9.txt` | every configuration on Silesia (with Zstandard's negative levels), Canterbury+Calgary, enwik8 and enwik9; eight runs per node |
| `epyc_checked_S.txt` | WZIP's and WLZ4's bounds-checked decoders on Silesia |
| `epyc_control_*.txt` | all of the above again, two runs per node: speeds within noise of the first run (median difference 0.0%) |
| `epyc_log.txt`, `epyc_control_log.txt` | node, core and seconds of each task |
| `epyc_zstd_asm.txt` | Zstandard's decompression with (`asm`) and without (`noasm`, `-DZSTD_DISABLE_ASM`) its assembly Huffman decoder, three interleaved passes from saved streams: levels -1 to 22 on Silesia, then 1 to 22 on enwik9; the assembly gains 0.1-2.2% |
| `epyc_blocks_S.txt`, `epyc_blocks_C.txt`, `epyc_control_blocks_*.txt` | `bench/bench_blocks`, 4 KB and 8 KB blocks, two runs (eight and two per node) |

A configuration measured more than once keeps its best speeds; its ratio and memory are the same in every run.

Threads (not in the paper's tables; the README's "Threads"):

| File | Contents |
|---|---|
| `epyc_threads.txt` | `bench/mt_find.c`: WZIP_L at levels 7, 9, 11 (also with an 8 MiB prefix dictionary) and 13 on Silesia file by file, and level 11 on enwik8, with 1 to 7 threads each, every run on one 8-core complex; every output identical to one thread's |
| `epyc_linked.txt` | `wzip -b` (frames in memory, best of the runs in a second) on whole nodes, threads unpinned: enwik9 at levels 0-13 and Silesia concatenated (`silesia.cat`, 211,938,580 bytes) at level 11, with 1 to 105 threads (one stream up to the threads one block uses, linked blocks of 64 MiB beyond); then one 64 MiB block of enwik9 with and without its 128 MiB dictionary, 1 to 7 threads on one complex (`bench/block_dict.c`); then decoding speed of one stream, independent and linked 64 MiB blocks (`bench/frame_dec.c`, core 4, best of 10) |

## Intel Core i7-8850H laptop (Windows 11, MSYS2 GCC 14.2)

The paper's first platform; the same ratios. Its tables were `gen_tables.py` applied to these files:

```sh
cd bench
python gen_tables.py OUT S=../results/final_S.txt,../results/final2_S.txt,../results/rerun_S.txt \
  C=../results/final_C.txt,../results/final2_C.txt,../results/rerun_C.txt,../results/rerun2_C.txt \
  E8=../results/e8c.txt E9=../results/e9c.txt \
  SX=../results/lz4class_S.txt,../results/lz4class_fast_S.txt,../results/lz4class_dec_S.txt \
  CX=../results/lz4class_C.txt,../results/lz4class_fast_C.txt,../results/lz4class_dec_C.txt \
  E8X=../results/lz4class_e8.txt,../results/lz4class_dec_e8.txt E9X=../results/lz4class_e9.txt,../results/lz4class_dec_e9.txt
```

(`bench/run_wlz4.sh` with `STAGES=TABLES` runs this and the figure.)

| File | Contents |
|---|---|
| `final_S.txt`, `final_C.txt` | every configuration of `runlist.txt` on Silesia and on Canterbury+Calgary |
| `final2_S.txt`, `final2_C.txt` | a second pass of the fast codecs (`runlist_fast.txt`) |
| `rerun_S.txt`, `rerun_C.txt` | WLZ4 levels 2, 4 and 6, rerun because another job disturbed their first timing |
| `rerun2_C.txt` | WLZ4 level 4 on Canterbury+Calgary, rerun for the same reason |
| `e8c.txt`, `e9c.txt` | enwik8 and enwik9, all measured in one later session |
| `lz4class_{S,C,fast_S,fast_C,e8,e9}.txt` | the LZ4 class, LZ4 and WLZ4, measured again in one session after WLZ4's 2026-10 format revision (`bench/run_wlz4.sh`, `COOL=62`); they replace every LZ4 and WLZ4 line of the files above, which hold the earlier format |
| `blocks_lz4class_S.txt`, `blocks_lz4class_C.txt` | the block benchmark's LZ4-class lines measured again after the revision (`bench/run_blocks.sh` with `LIST` and `W2=0`, see its header); they replace those lines of `blocks_*.txt` |
| `lz4class_dec_{S,C,e8,e9}.txt` | two more decompression-only passes over the same LZ4-class configurations, interleaved, from saved streams (compression speed and memory print as 0 and are not used) |
| `blocks_S.txt`, `blocks_C.txt` | `bench/bench_blocks` on Silesia and on Canterbury+Calgary cut into 4 KB and 8 KB blocks (README); `wzips-L2` is WZIP_S with length-2 matches within 16 bytes |
| `ablation_wzip.txt` | `bench/ablation/run.sh`: WZIP level 11 on Silesia under each ablation switch, every file verified (the paper's "WZIP's parts") |
| `ablation_wlz4.txt` | `bench/ablation/wlz4/run.sh`: the WLZ4 format variants of Table 5 (two-window, far code) and the "offset size by length alone" variants at levels 10 and 12, the original runs and a rerun from the repository sources (identical ratios), and the far-code parse's match mix |
| `position_S.txt`, `position_dec_S.txt`, `position_checked_S.txt` | not in the paper: LZ4, WLZ4 and Zstandard levels -7 to 9 on Silesia in one session (`bench/run_position.sh`, `COOL=62`), two more decompression-only passes, and WLZ4's bounds-checked decoder in the same passes; the README's "Where WLZ4 sits" figure and table |
| `zstd157.txt` | Zstandard 1.5.7 at levels 19 and 22, the paper's check that 1.5.7 changes the ratios by at most 0.06% |
