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
	corpus=$1; codec=$2; level=$3; rounds=$4
	eval "files=\$FILES_$corpus"
	start=$(date +%s)
	taskset -c "$CPU" ./bench_all "$codec" "$level" "$rounds" 20 $files > "$R/out/$corpus.$codec.$level.txt" 2> "$R/out/$corpus.$codec.$level.err"
	echo "$corpus $codec $level $(hostname) cpu$CPU $(( $(date +%s) - start ))s" >> "$R/out/$corpus.$codec.$level.txt"
done
