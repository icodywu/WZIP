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
# 1.1.0: filters (x86-like code, 16-bit samples, then text: x86 and delta filters, and a region left alone)
python - <<'PY'
import random
r = random.Random(2027)
ops = [b'\x8b\x45\xf8', b'\x89\x45\xfc', b'\x83\xc4\x08', b'\x50', b'\x5d\xc3', b'\x8d\x4d\xf0', b'\x33\xc0', b'\x85\xc0']
fns = [r.randrange(131072) for _ in range(300)]
code = bytearray()
while len(code) < 131072:
    if r.randrange(4) == 0:
        code += b'\xe8' + ((r.choice(fns) - (len(code) + 5)) & 0xFFFFFFFF).to_bytes(4, 'little')
    else:
        code += r.choice(ops)
samples, v, dv = bytearray(), 0, 0
for i in range(65536):
    dv = max(-200, min(200, dv + r.randrange(33) - 16))
    v += dv
    if abs(v) > 30000:
        v, dv = (30000 if v > 0 else -30000), -dv
    samples += (v & 0xFFFF).to_bytes(2, 'little')
open('filters.dat', 'wb').write(bytes(code[:131072]) + bytes(samples) + open('text.txt', 'rb').read()[:32768])
PY
"$W" -q -f -9 -c filters.dat > filters.dat.L9.wz
# 1.1.0: delta by a stride of its own (13-byte records, filter 10) and 12-bit joint codes (SDF-like text)
python - <<'PY'
import random
r = random.Random(2028)
out = bytearray()
t = 1_700_000_000; price = 10000
for i in range(16000):                       # 13-byte records: id (u32), time (u32), price (u16), qty (u16), flag (u8)
    t += r.randrange(1, 40); price += r.randrange(-30, 31)
    out += i.to_bytes(4, 'little') + t.to_bytes(4, 'little') + (price & 0xFFFF).to_bytes(2, 'little') + r.randrange(1, 500).to_bytes(2, 'little') + bytes([r.randrange(3)])
open('records.dat', 'wb').write(bytes(out))
r = random.Random(2029)
el = ["C", "C", "C", "C", "O", "N", "H", "H", "H", "S", "Cl"]
out, mol = [], 0
while sum(len(x) for x in out) < 150000:
    mol += 1
    n = r.randrange(6, 30); b = n - 1 + r.randrange(0, 3)
    out.append("%d\n  -OEChem-0926%02d%d2D\n\n%3d%3d  0     0  0  0  0  0  0999 V2000\n" % (mol, r.randrange(10, 20), r.randrange(10), n, b))
    for i in range(n):
        out.append("%10.4f%10.4f%10.4f %-3s 0  0  0  0  0  0  0  0  0  0  0  0\n" % (r.uniform(-5, 5), r.uniform(-5, 5), 0.0, r.choice(el)))
    for i in range(b):
        out.append("%3d%3d%3d  0  0  0  0\n" % (r.randrange(1, n + 1), r.randrange(1, n + 1), r.choice([1, 1, 1, 2])))
    out.append("M  END\n> <NSC>\n%d\n\n$$$$\n" % (r.randrange(1, 800000)))
open('sdf.dat', 'w', newline='\n').write(''.join(out))
PY
"$W" -q -f -11 -c records.dat > records.dat.L11.wz
"$W" -q -f -11 -c sdf.dat > sdf.dat.L11.wz
ls -l
