#!/bin/sh
# Re-measures WLZ4 alone (after its 2026-10 format revision) with the paper's settings and regenerates the tables and
# figure, the other codecs' results kept from results/. Same harness and runlists as run_paper.sh, WLZ4 lines only.
#   SILESIA, CANTERBURY, CALGARY, ENWIK: corpus directories (see README.md)
#   OUT: where the raw results go (default ../results); STAGES: any of S C E8 E9 TABLES (default all)
# Run nothing else meanwhile: the harness pins itself to one core and times whole-file calls.
set -e
cd "$(dirname "$0")"
: "${SILESIA:?set SILESIA}" "${CANTERBURY:?set CANTERBURY}" "${CALGARY:?set CALGARY}"
OUT=${OUT:-../results}
STAGES=${STAGES:-"S C E8 E9 TABLES"}
R=../results

S="$SILESIA/dickens $SILESIA/mozilla $SILESIA/mr $SILESIA/nci $SILESIA/ooffice $SILESIA/osdb $SILESIA/reymont"
S="$S $SILESIA/samba $SILESIA/sao $SILESIA/webster $SILESIA/x-ray $SILESIA/xml"
C="$(ls -d "$CANTERBURY"/* "$CALGARY"/* | tr '\n' ' ')"
has() { case " $STAGES " in *" $1 "*) return 0;; esac; return 1; }

if has S; then
	rm -f "$OUT/wlz4_S.txt" "$OUT/wlz4_fast_S.txt"
	grep '^wlz4' runlist.txt | while read -r codec level rounds; do ./bench_all.exe "$codec" "$level" "$rounds" 10 $S >> "$OUT/wlz4_S.txt"; done
	grep '^wlz4' runlist_fast.txt | while read -r codec level rounds; do ./bench_all.exe "$codec" "$level" "$rounds" 10 $S >> "$OUT/wlz4_fast_S.txt"; done
fi
if has C; then
	rm -f "$OUT/wlz4_C.txt" "$OUT/wlz4_fast_C.txt"
	grep '^wlz4' runlist.txt | while read -r codec level rounds; do ./bench_all.exe "$codec" "$level" "$rounds" 10 $C >> "$OUT/wlz4_C.txt"; done
	grep '^wlz4' runlist_fast.txt | while read -r codec level rounds; do ./bench_all.exe "$codec" "$level" "$rounds" 10 $C >> "$OUT/wlz4_fast_C.txt"; done
fi
if has E8 && [ -n "$ENWIK" ]; then
	rm -f "$OUT/wlz4_e8.txt"
	grep '^wlz4' runlist.txt | while read -r codec level rounds; do ./bench_all.exe "$codec" "$level" "$rounds" 10 "$ENWIK/enwik8" >> "$OUT/wlz4_e8.txt"; done
fi
if has E9 && [ -n "$ENWIK" ]; then
	rm -f "$OUT/wlz4_e9.txt"
	grep '^wlz4' runlist_e9.txt | while read -r codec level rounds; do ./bench_all.exe "$codec" "$level" "$rounds" 10 "$ENWIK/enwik9" >> "$OUT/wlz4_e9.txt"; done
fi
if has TABLES; then
	python gen_tables.py "$OUT" S=$R/final_S.txt,$R/final2_S.txt,$R/rerun_S.txt \
		C=$R/final_C.txt,$R/final2_C.txt,$R/rerun_C.txt,$R/rerun2_C.txt E8=$R/e8c.txt E9=$R/e9c.txt \
		SX="$OUT/wlz4_S.txt,$OUT/wlz4_fast_S.txt" CX="$OUT/wlz4_C.txt,$OUT/wlz4_fast_C.txt" \
		E8X="$OUT/wlz4_e8.txt" E9X="$OUT/wlz4_e9.txt"
	python plots.py "$OUT/silesia_tradeoff.pdf" $R/final_S.txt $R/final2_S.txt $R/rerun_S.txt \
		--replace "$OUT/wlz4_S.txt" "$OUT/wlz4_fast_S.txt"
fi
