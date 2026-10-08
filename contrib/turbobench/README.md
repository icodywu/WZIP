# WLZ4 and WZIP in TurboBench

[TurboBench](https://github.com/powturbo/TurboBench) is an in-memory benchmark of 100+ codecs, each built from a git
submodule; it publishes nightly results. `add_to_turbobench.py` adds four entries, once this repository is the
submodule `wzip`:

| TurboBench name | Codec | Levels |
|---|---|---|
| `wlz4` | WLZ4, lazy mode | - |
| `wlz4fast` | WLZ4, fast mode (acceleration) | 1-99 |
| `wlz4hc` | WLZ4, hash chains (0-7) and optimal parsing (8-12) | 0-12 |
| `wzip` | WZIP (`wzip_compress_mt`); `t#`: threads of levels 7-13, with the output of one thread | 0-13 |

They decompress with the bounds-checked decoders (`WLZ_Decompress`, `wzip_decompress`).

```sh
git clone https://github.com/powturbo/TurboBench && cd TurboBench
git submodule update --init                     # the other codecs
git submodule add https://github.com/icodywu/WZIP wzip && git -C wzip checkout v1.0.1
python ../WZIP/contrib/turbobench/add_to_turbobench.py .
make -j8
./turbobench -ewlz4/wlz4hc,2,12/lz4,12/zstd,1,19/wzip,1,11 silesia.tar
./turbobench -ewzip,11t6 silesia.tar
```

The script edits `plugin.cc` (codec ids, headers, registry, compression, decompression, versions, and freeing
WLZ4's tables in `codexit`) and the `makefile` (the build rule, `-O2 -DWZIP_MULTITHREAD=1`), keeping each file's
line endings (TurboBench keeps `plugin.cc` in CRLF). It was made against TurboBench `d12f18a` (2026-10-07); on a
later TurboBench it stops at the first anchor it no longer finds.

## Proposing them to TurboBench

They are proposed in [powturbo/TurboBench#59](https://github.com/powturbo/TurboBench/pull/59). The submodule must point at a public commit, best a release tag. Commit the submodule, `.gitmodules`, `makefile`
and `plugin.cc` as one change ("Add wlz4 and wzip 1.0.1"), push to a fork and open a pull request against
`powturbo/TurboBench`, as for lzbench (`../lzbench/README.md`).
