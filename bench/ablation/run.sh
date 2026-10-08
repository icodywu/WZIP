#!/bin/sh
# WZIP ablation of the DCC paper (Section 6, "WZIP's parts"): Silesia under each switch of switches.py, every level
# searching the full windows (WZ_FULLWIN=1): level 11 as is (base) and with each switch, and levels 12 and 13, which
# differ from 11 only in their parser (three states; then a first pass and eight offset groups).
# usage: sh bench/ablation/run.sh [silesia_dir] [config ...]    (from the repository root; needs gcc and python)
# Prints one line per file and a total per configuration. Ratios are deterministic; speeds are single runs.
set -e
DIR=${1:-data_corpus/SilesiaCorpus}; [ $# -gt 0 ] && shift
CONFIGS=${*:-"base classic ng1 ng5 onewin_ng5 l12 l13"}
B=${ABL_BUILD:-build/ablation}                     # a separate one per concurrent run
rm -rf $B && mkdir -p $B && cp src/*.c src/*.h $B/
python bench/ablation/switches.py $B/WZIP_L.c
LIBS=; case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) LIBS=-lpsapi;; esac
gcc -O2 -w -I$B bench/ablation/harness.c $B/WZIP_L.c $B/WZIP_M.c $B/WZIP_S.c $B/WZIP_wrapper.c $B/Huffman_Compress.c \
	$B/Huffman_Decompress.c $B/WLZ4.c -lm -pthread $LIBS -o $B/harness

for cfg in $CONFIGS; do
	level=11
	case $cfg in
		base) env=;; classic) env=WZ_CLASSIC=1;; ng1) env=WZ_NG=1;; ng5) env=WZ_NG=5;;
		onewin_ng5) env="WZ_ONEWIN=1 WZ_NG=5";; l12) env=; level=12;; l13) env=; level=13;;
		*) echo "unknown config $cfg"; exit 2;;
	esac
	tot=0; out=0; ok=1
	for f in dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml; do
		line=$(env WZ_FULLWIN=1 $env $B/harness $level "$DIR/$f") || ok=0
		echo "$cfg $line"
		n=$(echo "$line" | awk '{print $3, $5}')
		tot=$((tot + ${n% *})); out=$((out + ${n#* }))
	done
	echo "$cfg TOTAL in $tot out $out ratio $(awk "BEGIN {printf \"%.4f\", $tot / $out}") all_ok $ok"
done
