#!/bin/sh
# Re-measures the LZ4 class (LZ4 and WLZ4, in one session) after WLZ4's 2026-10 format revision, with the paper's
# settings, and regenerates the tables and figure, the other codecs' results kept from results/. Same harness and
# runlists as run_paper.sh, LZ4-class lines only.
#   SILESIA, CANTERBURY, CALGARY, ENWIK: corpus directories (see README.md)
#   OUT: where the raw results go (default ../results)
#   STAGES: any of S C E8 E9 PREP DEC TABLES (default all). S, C, E8, E9: full runs (bench_all, as run_paper.sh);
#        PREP: saves every configuration's streams in STREAMDIR (in parallel, untimed); DEC: PASSES (default 2) more
#        decompression-only passes over them, the configurations interleaved, into $TAG_dec_*.txt
#   CODECS: the runlist lines to run (grep pattern, default '^w\?lz4'; '^wlz4' for WLZ4 alone)
#   TAG: the results' file prefix (default lz4class): $TAG_S.txt, $TAG_fast_S.txt, $TAG_C.txt, $TAG_fast_C.txt,
#        $TAG_e8.txt, $TAG_e9.txt, $TAG_dec_{S,C,e8,e9}.txt; every codec they contain replaces that codec's earlier
#        results in the tables, and a configuration's speeds are its best over the runs
#   COOL: if set (degrees C), waits before each configuration until the CPU's thermal zone is that cool (Windows), so
#        that the heat of a long compression does not slow the next configuration (the paper's run used COOL=62)
#   STREAMDIR: for PREP and DEC, a directory with room for the streams (about 5 GB); JOBS: PREP's parallel jobs
#        (default 5; an enwik9 job takes 3 GB of memory)
# Run nothing else meanwhile: the harness pins itself to one core and times whole-file calls.
set -e
cd "$(dirname "$0")"
: "${SILESIA:?set SILESIA}" "${CANTERBURY:?set CANTERBURY}" "${CALGARY:?set CALGARY}"
OUT=${OUT:-../results}
STAGES=${STAGES:-"S C E8 E9 PREP DEC TABLES"}
CODECS=${CODECS:-'^w\?lz4'}
TAG=${TAG:-lz4class}
PASSES=${PASSES:-2}
R=../results

S="$SILESIA/dickens $SILESIA/mozilla $SILESIA/mr $SILESIA/nci $SILESIA/ooffice $SILESIA/osdb $SILESIA/reymont"
S="$S $SILESIA/samba $SILESIA/sao $SILESIA/webster $SILESIA/x-ray $SILESIA/xml"
C="$(ls -d "$CANTERBURY"/* "$CALGARY"/* | tr '\n' ' ')"
has() { case " $STAGES " in *" $1 "*) return 0;; esac; return 1; }
cool() {
	[ -n "$COOL" ] || return 0
	while :; do
		t=$(powershell -NoProfile -Command "[int]((Get-Counter '\Thermal Zone Information(*)\Temperature').CounterSamples[0].CookedValue - 273.15)" | tr -d '\r')
		[ -n "$t" ] || return 0
		[ "$t" -le "$COOL" ] && return 0
		sleep 10
	done
}
run() {   # runlist file-list output
	grep "$CODECS" "$1" | while read -r codec level rounds; do
		cool
		./bench_all.exe "$codec" "$level" "$rounds" 10 $2 >> "$3"
	done
}
# the decompression-only configurations: corpus tag, runlist, file list
configs() {
	printf 'S runlist.txt\nC runlist.txt\ne8 runlist.txt\ne9 runlist_e9.txt\n' | while read -r k list; do
		case $k in e8|e9) [ -n "$ENWIK" ] || continue;; esac
		grep "$CODECS" "$list" | while read -r codec level rounds; do echo "$k $codec $level"; done
	done
}
files() { case $1 in S) echo "$S";; C) echo "$C";; e8) echo "$ENWIK/enwik8";; e9) echo "$ENWIK/enwik9";; esac; }

if has S; then
	rm -f "$OUT/${TAG}_S.txt" "$OUT/${TAG}_fast_S.txt"
	run runlist.txt "$S" "$OUT/${TAG}_S.txt"
	run runlist_fast.txt "$S" "$OUT/${TAG}_fast_S.txt"
fi
if has C; then
	rm -f "$OUT/${TAG}_C.txt" "$OUT/${TAG}_fast_C.txt"
	run runlist.txt "$C" "$OUT/${TAG}_C.txt"
	run runlist_fast.txt "$C" "$OUT/${TAG}_fast_C.txt"
fi
if has E8 && [ -n "$ENWIK" ]; then
	rm -f "$OUT/${TAG}_e8.txt"
	run runlist.txt "$ENWIK/enwik8" "$OUT/${TAG}_e8.txt"
fi
if has E9 && [ -n "$ENWIK" ]; then
	rm -f "$OUT/${TAG}_e9.txt"
	run runlist_e9.txt "$ENWIK/enwik9" "$OUT/${TAG}_e9.txt"
fi
if has PREP; then
	: "${STREAMDIR:?set STREAMDIR}"
	mkdir -p "$STREAMDIR"
	configs | tac | {                                  # the slowest (enwik9) first, JOBS at a time
		while read -r k codec level; do
			while [ "$(jobs -rp | wc -l)" -ge "${JOBS:-5}" ]; do sleep 2; done
			STREAMS="$STREAMDIR" STREAMS_ONLY=1 ./bench_all.exe "$codec" "$level" 1 1 $(files $k) &
		done
		wait
	}
fi
if has DEC; then
	: "${STREAMDIR:?set STREAMDIR}"
	rm -f "$OUT/${TAG}_dec_S.txt" "$OUT/${TAG}_dec_C.txt" "$OUT/${TAG}_dec_e8.txt" "$OUT/${TAG}_dec_e9.txt"
	p=0
	while [ $p -lt "$PASSES" ]; do
		configs | while read -r k codec level; do
			cool
			STREAMS="$STREAMDIR" ./bench_all.exe "$codec" "$level" 1 10 $(files $k) >> "$OUT/${TAG}_dec_$k.txt"
		done
		p=$((p + 1))
	done
fi
if has TABLES; then
	opt() { [ -f "$1" ] && echo ",$1" || true; }
	python gen_tables.py "$OUT" S=$R/final_S.txt,$R/final2_S.txt,$R/rerun_S.txt \
		C=$R/final_C.txt,$R/final2_C.txt,$R/rerun_C.txt,$R/rerun2_C.txt E8=$R/e8c.txt E9=$R/e9c.txt \
		SX="$OUT/${TAG}_S.txt,$OUT/${TAG}_fast_S.txt$(opt "$OUT/${TAG}_dec_S.txt")" \
		CX="$OUT/${TAG}_C.txt,$OUT/${TAG}_fast_C.txt$(opt "$OUT/${TAG}_dec_C.txt")" \
		E8X="$OUT/${TAG}_e8.txt$(opt "$OUT/${TAG}_dec_e8.txt")" E9X="$OUT/${TAG}_e9.txt$(opt "$OUT/${TAG}_dec_e9.txt")"
	python plots.py "$OUT/silesia_tradeoff.pdf" $R/final_S.txt $R/final2_S.txt $R/rerun_S.txt \
		--replace "$OUT/${TAG}_S.txt" "$OUT/${TAG}_fast_S.txt" $([ -f "$OUT/${TAG}_dec_S.txt" ] && echo "$OUT/${TAG}_dec_S.txt")
fi
