#!/bin/sh
# Block benchmark of the README: every file cut into independent 4 KB and 8 KB blocks.
#   SILESIA, CANTERBURY, CALGARY: corpus directories; OUT: output directory (default ../results/new)
#   LIST: the configurations (codec:level:rounds); W2=0 skips the length-2 experiment. The LZ4 class was measured
#   again after WLZ4's 2026-10 format revision with LIST="lz4:1:3 lz4hc:9:1 lz4hc:12:1 wlz4f:1:3 wlz4hc:2:1
#   wlz4hc:10:1" W2=0 (results/blocks_lz4class_*.txt, which replace those lines of results/blocks_*.txt)
# Builds bench_blocks.exe and, for the length-2 experiment, bench_blocks_w2.exe (WZIP_S with S_W2_BITS = 4).
set -e
cd "$(dirname "$0")"
: "${SILESIA:?set SILESIA}" "${CANTERBURY:?set CANTERBURY}" "${CALGARY:?set CALGARY}"
OUT=${OUT:-../results/new}
mkdir -p "$OUT"
[ "${W2:-1}" = 0 ] || [ -x bench_blocks_w2.exe ] || { make -s S_W2=4 bench_blocks.exe && mv bench_blocks.exe bench_blocks_w2.exe; }
[ -x bench_blocks.exe ] || make -s bench_blocks.exe

S="$SILESIA/dickens $SILESIA/mozilla $SILESIA/mr $SILESIA/nci $SILESIA/ooffice $SILESIA/osdb $SILESIA/reymont"
S="$S $SILESIA/samba $SILESIA/sao $SILESIA/webster $SILESIA/x-ray $SILESIA/xml"
C="$(ls -d "$CANTERBURY"/* "$CALGARY"/* | tr '\n' ' ')"
LIST=${LIST:-"lz4:1:3 lz4hc:9:1 lz4hc:12:1 wlz4f:1:3 wlz4hc:2:1 wlz4hc:10:1 zstd:1:3 zstd:3:3 zstd:9:1 zstd:19:1 wzips:1:3 wzips:5:1 wzips:9:1 wzipm:9:1 wzipm:12:1"}

for corpus in C S; do
	if [ $corpus = C ]; then FILES="$C"; else FILES="$S"; fi
	rm -f "$OUT/blocks_$corpus.txt"
	for bs in 4096 8192; do
		for e in $LIST; do
			c=${e%%:*}; r=${e##*:}; l=${e#*:}; l=${l%%:*}
			./bench_blocks.exe $c $l $bs $r $FILES >> "$OUT/blocks_$corpus.txt"
		done
		# WZIP_S with length-2 matches within 16 bytes (S_W2_BITS = 4)
		[ "${W2:-1}" = 0 ] || for l in 5 9; do ./bench_blocks_w2.exe wzips $l $bs 1 $FILES | sed 's/^wzips  /wzips-L2/' >> "$OUT/blocks_$corpus.txt"; done
	done
done
