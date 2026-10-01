#!/bin/sh
# WLZ4 ablation of the DCC paper (Table 5 and the "offset size by length alone" sentence of Section 5): WLZ4 as of
# 2026-09-29 (two-window format) and its match-code variants, levels 10 and 12, on Silesia and Canterbury+Calgary.
# usage: sh bench/ablation/wlz4/run.sh [data_corpus]      (from the repository root; MSYS2 MinGW-w64 gcc, Windows)
# LZ4 sources (for the harness's lz4 codecs) default to bench/deps/lz4/lib (cd bench && make deps); override with LZ4=.
# Prints one line per build, corpus and level; ratios are deterministic, speeds are not used.
set -e
D=${1:-data_corpus}
LZ4=${LZ4:-bench/deps/lz4/lib}
A=bench/ablation/wlz4
B=${ABL_BUILD:-build/ablation_wlz4}
rm -rf $B && mkdir -p $B
F="-O2 -w -I$A -Isrc -I$LZ4 -include stdlib.h -Dmax(a,b)=(((a)>(b))?(a):(b)) -Dmin(a,b)=(((a)<(b))?(a):(b))"
gcc $F -c $LZ4/lz4.c -o $B/lz4.o && gcc $F -c $LZ4/lz4hc.c -o $B/lz4hc.o
gcc $F -c $A/WLZ4_twowin.c -o $B/twowin.o
gcc $F $A/w4benchfull.c $B/twowin.o $B/lz4.o $B/lz4hc.o -o $B/b_twowin
for v in 0 1 2; do
	gcc $F -DWLZ_VARIANT=$v -c $A/WLZ4_variants.c -o $B/v$v.o
	gcc $F $A/w4benchfull.c $B/v$v.o $B/lz4.o $B/lz4hc.o -o $B/b_v$v
done

SIL=; for f in dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml; do SIL="$SIL $D/SilesiaCorpus/$f"; done
SMALL=$(ls -d $D/CanterburyCorpus/* $D/CalgaryCorpus/* | tr '\n' ' ')
for b in twowin v0 v1 v2; do
	for L in 10 12; do
		echo "$b small $(W4MAXN=52000000 $B/b_$b wlzhc $L 1 $SMALL | tail -1)"
		echo "$b silesia $(W4MAXN=52000000 $B/b_$b wlzhc $L 1 $SIL | tail -1)"
	done
done

# the far-code parse's match mix at level 10 (Section 5: the share of matches of length 6+ within 64 KB)
gcc $F $A/v3stat.c $B/v0.o -o $B/v3stat
echo "match mix, small files:"; $B/v3stat 10 $SMALL
echo "match mix, Silesia:"; $B/v3stat 10 $SIL
