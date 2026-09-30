#!/bin/sh
# Reruns the measurements of the paper and regenerates its tables and figure.
#   SILESIA, CANTERBURY, CALGARY, ENWIK: corpus directories (see README.md)
#   OUT: where the raw results go (default ../results/new)
# Run nothing else meanwhile: the harness pins itself to one core and times whole-file calls.
set -e
cd "$(dirname "$0")"
: "${SILESIA:?set SILESIA}" "${CANTERBURY:?set CANTERBURY}" "${CALGARY:?set CALGARY}"
OUT=${OUT:-../results/new}
mkdir -p "$OUT"

S="$SILESIA/dickens $SILESIA/mozilla $SILESIA/mr $SILESIA/nci $SILESIA/ooffice $SILESIA/osdb $SILESIA/reymont"
S="$S $SILESIA/samba $SILESIA/sao $SILESIA/webster $SILESIA/x-ray $SILESIA/xml"
C="$(ls -d "$CANTERBURY"/* "$CALGARY"/* | tr '\n' ' ')"

# every configuration once (compression rounds as listed), then a second pass of the fast codecs
rm -f "$OUT"/final_S.txt "$OUT"/final_C.txt "$OUT"/final2_S.txt "$OUT"/final2_C.txt
while read -r codec level rounds; do
	./bench_all.exe "$codec" "$level" "$rounds" 10 $S >> "$OUT/final_S.txt"
	./bench_all.exe "$codec" "$level" "$rounds" 10 $C >> "$OUT/final_C.txt"
done < runlist.txt
while read -r codec level rounds; do
	./bench_all.exe "$codec" "$level" "$rounds" 10 $S >> "$OUT/final2_S.txt"
	./bench_all.exe "$codec" "$level" "$rounds" 10 $C >> "$OUT/final2_C.txt"
done < runlist_fast.txt

# large inputs, in one session
if [ -n "$ENWIK" ]; then
	rm -f "$OUT"/e8c.txt "$OUT"/e9c.txt
	while read -r codec level rounds; do ./bench_all.exe "$codec" "$level" "$rounds" 10 "$ENWIK/enwik8" >> "$OUT/e8c.txt"; done < runlist.txt
	while read -r codec level rounds; do ./bench_all.exe "$codec" "$level" "$rounds" 10 "$ENWIK/enwik9" >> "$OUT/e9c.txt"; done < runlist_e9.txt
	E="E8=$OUT/e8c.txt E9=$OUT/e9c.txt"
fi

python gen_tables.py "$OUT" S="$OUT/final_S.txt,$OUT/final2_S.txt" C="$OUT/final_C.txt,$OUT/final2_C.txt" $E
python plots.py "$OUT/silesia_tradeoff.pdf" "$OUT/final_S.txt" "$OUT/final2_S.txt"
