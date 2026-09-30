# Raw results of the paper

Output of `bench/bench_all` (one line per codec and level: ratio, compression and decompression speed in MB/s,
encoder memory in MB, verification failures). The paper's tables are `gen_tables.py` applied to these files:

```sh
cd bench
python gen_tables.py OUT S=../results/final_S.txt,../results/final2_S.txt,../results/rerun_S.txt \
  C=../results/final_C.txt,../results/final2_C.txt,../results/rerun_C.txt,../results/rerun2_C.txt \
  E8=../results/e8c.txt E9=../results/e9c.txt
```

| File | Contents |
|---|---|
| `final_S.txt`, `final_C.txt` | every configuration of `runlist.txt` on Silesia and on Canterbury+Calgary |
| `final2_S.txt`, `final2_C.txt` | a second pass of the fast codecs (`runlist_fast.txt`) |
| `rerun_S.txt`, `rerun_C.txt` | WLZ4 levels 2, 4 and 6, rerun because another job disturbed their first timing |
| `rerun2_C.txt` | WLZ4 level 4 on Canterbury+Calgary, rerun for the same reason |
| `e8c.txt`, `e9c.txt` | enwik8 and enwik9, all measured in one later session |

A configuration measured more than once keeps its best speeds; its ratio and memory are the same in every run.
