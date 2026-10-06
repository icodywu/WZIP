#!/bin/sh
# Coverage-guided fuzzing of every decoder and of the round trip, with libFuzzer, ASan and UBSan.
# usage (from the repository root): sh tests/fuzz/run.sh <work directory> [seconds per target] [jobs per target]
# Needs a clang with libFuzzer (CC, default clang). The seven targets run at once, each with its jobs in libFuzzer's
# fork mode; corpora grow in <work>/<target>/corpus, and findings are left as <work>/<target>/crash-* (reproduce one
# with <work>/fuzz_<target> <file>).
set -e
W=$1; T=${2:-600}; J=${3:-4}; CC=${CC:-clang}
# ASan's global instrumentation is off (-asan-globals=0): with ROCm's clang 18 it misplaced some static const tables
# and aborted at start-up; UBSan's bounds checks still cover indexing into those tables.
FLAGS="-g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -mllvm -asan-globals=0 -Isrc"
SRC="src/WZIP_L.c src/WZIP_M.c src/WZIP_S.c src/WZIP_wrapper.c src/Huffman_Compress.c src/Huffman_Decompress.c src/WLZ4.c src/wzframe.c"
NAMES="frame wzip wlz4 wlz4dict wzips wzipsdict"
mkdir -p "$W"
k=0
for t in $NAMES; do
	$CC $FLAGS -DFUZZ_TARGET=$k tests/fuzz/fuzz_decode.c $SRC -lm -o "$W/fuzz_$t"
	mkdir -p "$W/$t/corpus"
	k=$((k + 1))
done
$CC $FLAGS tests/fuzz/fuzz_roundtrip.c $SRC -lm -o "$W/fuzz_roundtrip"
mkdir -p "$W/roundtrip/corpus"
# seeds: bare streams, frames and inputs made from the golden inputs, and the golden frames themselves
$CC -O2 -Isrc tests/fuzz/make_seeds.c $SRC -lm -o "$W/make_seeds"
"$W/make_seeds" "$W" tests/golden/small.txt tests/golden/text.txt tests/golden/bin.dat
cp tests/golden/*.wz tests/golden/*.wlz4 "$W/frame/corpus/"
# inputs that once failed, named <target>-<what>, so that every run tries them first
for f in tests/fuzz/regressions/*; do t=$(basename "$f"); cp "$f" "$W/${t%%-*}/corpus/"; done
for t in $NAMES roundtrip; do
	(cd "$W/$t" && "../fuzz_$t" -max_total_time="$T" -fork="$J" -ignore_crashes=1 -max_len=262144 \
	    -rss_limit_mb=4096 -timeout=60 corpus > log.txt 2>&1 || true) &
done
wait
total=0
for t in $NAMES roundtrip; do
	n=$(ls "$W/$t" | grep -c -E '^(crash|timeout|leak|oom)-' || true)
	total=$((total + n))
	echo "$t: $n findings, corpus $(ls "$W/$t/corpus" | wc -l) inputs; $(grep -a -o 'exec/s: [0-9]*' "$W/$t/log.txt" | tail -1)"
done
[ $total -eq 0 ]
