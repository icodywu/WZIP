#!/bin/sh
# Where WLZ4 sits between LZ4 and Zstandard (README.md): LZ4, LZ4HC, WLZ4 and Zstandard's fast levels, its negative
# ("--fast") levels included, measured on Silesia in one session, then the figure doc/figures/wlz4-position.svg.
#   SILESIA: the corpus directory (see README.md); OUT: where the raw results go (default ../results)
#   STAGES: any of FULL PREP DEC FIG (default all). FULL: bench_all over runlist_position.txt into position_S.txt;
#        PREP: saves every configuration's streams in STREAMDIR (in parallel, untimed); DEC: PASSES (default 2) more
#        decompression-only passes over them, the configurations interleaved, into position_dec_S.txt, and for WLZ4
#        the same with its bounds-checked decoder (CHECKED=1) into position_checked_S.txt; FIG: the figure, and the
#        README table on standard output
#   COOL: if set (degrees C), waits before each configuration until the CPU's thermal zone is that cool (Windows);
#        the README's run used COOL=62
#   STREAMDIR: for PREP and DEC, a directory with room for the streams (about 1.5 GB); JOBS: PREP's parallel jobs
#        (default 5)
# Run nothing else meanwhile: the harness pins itself to one core and times whole-file calls.
set -e
cd "$(dirname "$0")"
: "${SILESIA:?set SILESIA}"
OUT=${OUT:-../results}
STAGES=${STAGES:-"FULL PREP DEC FIG"}
PASSES=${PASSES:-2}

S="$SILESIA/dickens $SILESIA/mozilla $SILESIA/mr $SILESIA/nci $SILESIA/ooffice $SILESIA/osdb $SILESIA/reymont"
S="$S $SILESIA/samba $SILESIA/sao $SILESIA/webster $SILESIA/x-ray $SILESIA/xml"
has() { case " $STAGES " in *" $1 "*) return 0;; esac; return 1; }
cool() {
	[ -n "$COOL" ] || return 0
	while :; do
		t=$(powershell -NoProfile -Command "[int]((Get-Counter '\Thermal Zone Information(*)\Temperature').CounterSamples[0].CookedValue - 273.15)" < /dev/null | tr -d '\r')
		[ -n "$t" ] || return 0
		[ "$t" -le "$COOL" ] && return 0
		sleep 10
	done
}

if has FULL; then
	rm -f "$OUT/position_S.txt"
	while read -r codec level rounds; do
		cool
		./bench_all.exe "$codec" "$level" "$rounds" 10 $S >> "$OUT/position_S.txt"
	done < runlist_position.txt
fi
if has PREP; then
	: "${STREAMDIR:?set STREAMDIR}"
	mkdir -p "$STREAMDIR"
	while read -r codec level rounds; do
		while [ "$(jobs -rp | wc -l)" -ge "${JOBS:-5}" ]; do sleep 2; done
		STREAMS="$STREAMDIR" STREAMS_ONLY=1 ./bench_all.exe "$codec" "$level" 1 1 $S &
	done < runlist_position.txt
	wait
fi
if has DEC; then
	: "${STREAMDIR:?set STREAMDIR}"
	rm -f "$OUT/position_dec_S.txt" "$OUT/position_checked_S.txt"
	p=0
	while [ $p -lt "$PASSES" ]; do
		while read -r codec level rounds; do
			cool
			STREAMS="$STREAMDIR" ./bench_all.exe "$codec" "$level" 1 10 $S >> "$OUT/position_dec_S.txt"
			case $codec in wlz4*)
				cool
				CHECKED=1 STREAMS="$STREAMDIR" ./bench_all.exe "$codec" "$level" 1 10 $S >> "$OUT/position_checked_S.txt";;
			esac
		done < runlist_position.txt
		p=$((p + 1))
	done
fi
if has FIG; then
	python plot_position.py ../doc/figures/wlz4-position.svg "$OUT/position_S.txt" \
		--dec "$OUT/position_dec_S.txt" --checked "$OUT/position_checked_S.txt"
fi
