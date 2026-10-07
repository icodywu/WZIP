# wzip

Python bindings of [WZIP and WLZ4](https://github.com/icodywu/WZIP), two LZ77 codecs with match-length-dependent
sliding windows: WZIP is an entropy-coded codec of the Zstandard class; WLZ4 a byte-aligned codec of the LZ4 class,
which on Silesia reaches Zstandard 3's ratio and decodes twice as fast.

```python
import wzip

packed = wzip.compress(data)                     # WZIP, level 1 (0-13; 7-13 optimal parsing)
packed = wzip.compress(data, "wlz4", level=10)   # WLZ4 (0-12, or "fast", "lazy")
packed = wzip.compress(data, level=11, threads=16)   # 16 threads (linked blocks if over 64 MiB)
assert wzip.decompress(packed) == data           # either codec; wzip.Error on damaged input

with wzip.open("notes.txt.wz", "wb") as f:       # files as the wzip and wlz4 tools write them
    f.write(data)
assert wzip.open("notes.txt.wz") == data
```

`compress` writes a WZ frame (magic number, codec, format version, content size, XXH32 checksum), the format of the
`wzip` and `wlz4` command-line tools. `wzip_compress`/`wzip_decompress` and `wlz4_compress`/`wlz4_decompress` give
the bare codec streams. Compression and decompression release the GIL. With `threads`, WZIP's levels 7-13 search a
block with up to 7 threads, with the frame of one thread; more threads cut WZIP content over 64 MiB into linked
blocks, each referring to the content before it, compressed at once and almost as well as one block. `decompress`
reads any frame without being told how it was made. BSD 2-Clause license.
