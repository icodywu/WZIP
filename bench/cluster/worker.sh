#!/bin/sh
# One benchmark slot: claims tasks from the shared queue (an atomic mkdir per task line) and runs each with bench_all
# pinned to one core.   usage: worker.sh <run directory> <core>
# The run directory holds queue.txt, corpora.sh (corpus file lists) and gets claims/, out/ (one result file per task).
R=$1; CPU=$2
cd "$(dirname "$0")/.."
. "$R/corpora.sh"
n=$(wc -l < "$R/queue.txt")
i=0
while [ $i -lt "$n" ]; do
	i=$((i + 1))
	mkdir "$R/claims/$i" 2>/dev/null || continue
	set -- $(sed -n "${i}p" "$R/queue.txt")
	if [ "$1" = B ]; then                                       # B corpus block-size codec level rounds: bench_blocks
		eval "files=\$FILES_$2"
		taskset -c "$CPU" ./bench_blocks "$4" "$5" "$3" "$6" $files > "$R/out/B.$2.$3.$4.$5.txt" 2> "$R/out/B.$2.$3.$4.$5.err"
		continue
	fi
	corpus=$1; codec=$2; level=$3; rounds=$4; mode=${5:-}       # mode "checked": the bounds-checked decoders
	eval "files=\$FILES_$corpus"
	name="$corpus.$codec.$level${mode:+.$mode}"
	start=$(date +%s)
	if [ "$mode" = checked ]; then export CHECKED=1; else unset CHECKED; fi
	taskset -c "$CPU" ./bench_all "$codec" "$level" "$rounds" 20 $files > "$R/out/$name.txt" 2> "$R/out/$name.err"
	echo "$corpus $codec $level${mode:+ $mode} $(hostname) cpu$CPU $(( $(date +%s) - start ))s" >> "$R/out/$name.txt"
done
