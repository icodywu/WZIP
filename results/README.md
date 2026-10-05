# Raw results of the paper

Output of `bench/bench_all` (one line per codec and level: ratio, compression and decompression speed in MB/s,
encoder memory in MB, verification failures). WZIP and WLZ4 were measured with the decoders that are now their opt-in
trusted mode (`wzip_decompress_trusted`, `WLZ_Decompress_Trusted`), the same code. The paper's tables are
`gen_tables.py` applied to these files:

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
| `zstd157.txt` | Zstandard 1.5.7 at levels 19 and 22, the paper's check that 1.5.7 changes the ratios by at most 0.06% |

A configuration measured more than once keeps its best speeds; its ratio and memory are the same in every run.
