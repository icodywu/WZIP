#!/bin/sh
# Adds WLZ4 (wlz4, wlz4fast, wlz4hc) and WZIP (wzip) to an lzbench checkout, or updates them to this repository's
# version where lzbench has them already (from commit 9ca711c, 2026-10-09): sh add_to_lzbench.sh path/to/lzbench
# Copies the codec sources from this repository. To add, it writes their mk files and applies lzbench.patch (the
# benchmark glue, the codec table, the aliases, README and CHANGELOG entries; made against lzbench commit 8bd060c,
# 2026-10-07); to update, it sets the version and release date in the codec table, README and CHANGELOG.
set -e
L=$(cd "$1" && pwd)
R=$(cd "$(dirname "$0")/../.." && pwd)
V=$(sed -n 's/^#define WZIP_VERSION_STRING *"\(.*\)".*/\1/p' "$R/src/WZIP.h")
D=$(sed -n "s/^## $V (\(.*\))\$/\1/p" "$R/CHANGELOG.md")
[ -n "$V" ] && [ -n "$D" ] || { echo "no version or release date for $V in CHANGELOG.md" >&2; exit 1; }
mkdir -p "$L/lz/wlz4" "$L/lz+entropy/wzip"
cp "$R/src/WLZ4.c" "$R/src/WLZ4.h" "$R/src/Memry.h" "$R/LICENSE" "$R/NOTICE" "$L/lz/wlz4/"
cp "$R/src/WZIP.h" "$R/src/WZIP_L.c" "$R/src/WZIP_M.c" "$R/src/WZIP_wrapper.c" "$R/src/Huffman_Compress.c" \
   "$R/src/Huffman_Decompress.c" "$R/src/BitStream_Huffman.h" "$R/src/Memry.h" "$R/src/wz_threads.h" \
   "$R/LICENSE" "$R/NOTICE" "$L/lz+entropy/wzip/"
if grep -q '"wzip ' "$L/bench/lzbench.h"; then
    sed -i -e "s/\"wlz4 [0-9.]*\( --fast\)\{0,1\}\"/\"wlz4 $V\1\"/" -e "s/\"wlz4hc [0-9.]*\"/\"wlz4hc $V\"/" \
           -e "s/\"wzip [0-9.]*\"/\"wzip $V\"/" "$L/bench/lzbench.h"
    sed -i "s#^| \[wlz4/wlz4hc [0-9.]*, wzip [0-9.]*\](https://github.com/icodywu/WZIP) | [0-9-]* |#| [wlz4/wlz4hc $V, wzip $V](https://github.com/icodywu/WZIP) | $D |#" "$L/README.md"
    # the line that added them while its lzbench version is in progress, else a new line at the top
    if sed -n '2,/^v[0-9]/p' "$L/CHANGELOG" | grep -q '^- added wlz4 '; then
        sed -i "2,/^v[0-9]/s/^- added wlz4 [0-9.]* (wlz4, wlz4fast, wlz4hc) and wzip [0-9.]*/- added wlz4 $V (wlz4, wlz4fast, wlz4hc) and wzip $V/" "$L/CHANGELOG"
    else
        sed -i "1a - updated wlz4 and wzip to $V (by @icodywu)" "$L/CHANGELOG"
    fi
    echo "updated wlz4 and wzip in $L to $V"
    exit 0
fi
printf '# wlz4\nCODECS += WLZ4\nWLZ4_OBJS := lz/wlz4/WLZ4.o\nWLZ4_OPT := O2\n' > "$L/mk/wlz4.mk"
cat > "$L/mk/wzip.mk" <<'MK'
# wzip
CODECS += WZIP
WZIP_OBJS := $(addprefix lz+entropy/wzip/, \
    WZIP_L.o WZIP_M.o WZIP_wrapper.o Huffman_Compress.o Huffman_Decompress.o)
WZIP_OPT := O2

# levels 7-13 find matches in threads of their own with -I# (the output is that of one thread)
ifneq ($(DISABLE_THREADING),1)
    WZIP_FLAGS := -DWZIP_MULTITHREAD=1
endif
MK
(cd "$L" && git apply "$R/contrib/lzbench/lzbench.patch")
echo "added wlz4 and wzip to $L; build with make, then e.g. ./lzbench -ewlz4/wlz4hc,2,10/wzip,1,11 file"
