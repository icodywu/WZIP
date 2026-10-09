#!/bin/sh
# Writes the golden inputs and frames: sh tests/golden/make.sh <directory holding wzip and wlz4>
# Run only to add new golden frames: existing ones must stay as they are, since every later version must decode them.
set -e
B=$(cd "$1" && pwd); EXE=; [ -f "$B/wzip.exe" ] && EXE=.exe
cd "$(dirname "$0")"
W="$B/wzip$EXE"; L="$B/wlz4$EXE"
python - <<'PY'
import random
r = random.Random(2026)
words = 'the of and a to in is was that for on as with by match window length offset code LZ77 Huffman frame block'.split()
def text(n):
    out = []
    while sum(map(len, out)) < n:
        out.append(' '.join(r.choice(words) for _ in range(r.randint(4, 14))).capitalize() + '.\n')
    return ''.join(out)[:n].encode()
open('text.txt', 'wb').write(text(49152))
open('small.txt', 'wb').write(text(6000))
b = bytearray()
while len(b) < 40000:            # structured records with random fields, and a random stretch
    b += bytes([r.randrange(256) for _ in range(3)]) + b'\x00\x01' + r.choice(words).encode() * 2
b[20000:24000] = bytes(r.randrange(256) for _ in range(4000))
open('bin.dat', 'wb').write(bytes(b[:40000]))
PY
"$W" -q -f -1 -c text.txt > text.txt.L1.wz
"$W" -q -f -11 -c text.txt > text.txt.L11.wz
"$W" -q -f -13 -B10 -c text.txt > text.txt.L13.B10.wz
"$W" -q -f -5 -c small.txt > small.txt.L5.wz
"$W" -q -f -3 --no-check -c bin.dat > bin.dat.L3.wz
"$L" -q -f -c small.txt > small.txt.lazy.wlz4
"$L" -q -f -10 -c text.txt > text.txt.L10.wlz4
"$L" -q -f --fast -B12 -c text.txt > text.txt.fast.B12.wlz4
"$L" -q -f -2 -B10 -c bin.dat > bin.dat.L2.B10.wlz4
{ "$W" -q -c small.txt; printf 'P*M\030\004\000\000\000meta'; "$L" -q -12 -c text.txt; } > concat.wz
cat small.txt text.txt > concat.ref
# 1.1.0: WZIP format 2, sized sequence blocks (text, records, text: blocks of several sizes)
cat text.txt bin.dat text.txt > mixed.dat
"$W" -q -f -9 -c mixed.dat > mixed.dat.L9.wz
"$W" -q -f -13 -B16 -c mixed.dat > mixed.dat.L13.B16.wz
ls -l
