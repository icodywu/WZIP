#!/bin/sh
# Adds WLZ4 (wlz4, wlz4fast, wlz4hc) and WZIP (wzip) to an lzbench checkout: sh add_to_lzbench.sh path/to/lzbench
# Copies the codec sources from this repository, writes their mk files, and applies lzbench.patch (the benchmark glue,
# the codec table, the aliases, README and CHANGELOG entries). Made against lzbench commit 1db5b9b (2026-10-02).
set -e
L=$(cd "$1" && pwd)
R=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$L/lz/wlz4" "$L/lz+entropy/wzip"
cp "$R/src/WLZ4.c" "$R/src/WLZ4.h" "$R/src/Memry.h" "$R/LICENSE" "$R/NOTICE" "$L/lz/wlz4/"
cp "$R/src/WZIP.h" "$R/src/WZIP_L.c" "$R/src/WZIP_M.c" "$R/src/WZIP_wrapper.c" "$R/src/Huffman_Compress.c" \
   "$R/src/Huffman_Decompress.c" "$R/src/BitStream_Huffman.h" "$R/src/Memry.h" "$R/LICENSE" "$L/lz+entropy/wzip/"
printf '# wlz4\nCODECS += WLZ4\nWLZ4_OBJS := lz/wlz4/WLZ4.o\nWLZ4_OPT := O2\n' > "$L/mk/wlz4.mk"
cat > "$L/mk/wzip.mk" <<'MK'
# wzip
CODECS += WZIP
WZIP_OBJS := $(addprefix lz+entropy/wzip/, \
    WZIP_L.o WZIP_M.o WZIP_wrapper.o Huffman_Compress.o Huffman_Decompress.o)
WZIP_OPT := O2
MK
(cd "$L" && git apply "$R/contrib/lzbench/lzbench.patch")
echo "added wlz4 and wzip to $L; build with make, then e.g. ./lzbench -ewlz4/wlz4hc,2,10/wzip,1,11 file"
