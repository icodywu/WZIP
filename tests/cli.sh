#!/bin/sh
# Test of the command-line tool: sh tests/cli.sh <directory holding wzip and wlz4>
# Round trips through files and pipes with both codecs, then the error cases: refusals, damaged and truncated files.
BIN=$(cd "$1" && pwd)
EXE=; [ -f "$BIN/wzip.exe" ] && EXE=.exe
WZIP="$BIN/wzip$EXE"; WLZ4="$BIN/wlz4$EXE"
T=$(mktemp -d 2>/dev/null || echo "${TMPDIR:-/tmp}/wzip-cli-$$")
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT
fails=0; checks=0
ok()  { checks=$((checks + 1)); }
bad() { checks=$((checks + 1)); fails=$((fails + 1)); echo "FAIL: $*"; }
same() { cmp -s "$1" "$2" && ok || bad "$3: $1 differs from $2"; }

# inputs: text-like, binary-like, empty, one byte, and a 3 MB file
i=0; : > "$T/text"
while [ $i -lt 400 ]; do echo "the match window length $i offset of a and LZ77 code $((i * 7 % 13))" >> "$T/text"; i=$((i + 1)); done
for k in 1 2 3 4 5 6 7 8; do cat "$T/text"; done > "$T/text8"
for k in 1 2 3 4 5 6 7 8 9 10; do cat "$T/text8"; done > "$T/big"
for k in 1 2 3 4 5 6 7 8 9 10 11 12; do cat "$T/big"; done > "$T/big3m"
: > "$T/empty"; printf x > "$T/one"
od -An -tx1 "$T/text8" | head -c 60000 > "$T/hex"

for f in text8 big3m empty one hex; do
	for codec in wzip wlz4; do
		[ $codec = wzip ] && { C="$WZIP"; X=wz; } || { C="$WLZ4"; X=wlz4; }
		for lv in "" -0 -9; do
			rm -f "$T/$f.$X" "$T/$f.out"
			"$C" -q $lv "$T/$f" || bad "$codec $lv $f: compression failed"
			[ -f "$T/$f.$X" ] && ok || bad "$codec $lv $f: no $f.$X"
			"$C" -q -t "$T/$f.$X" && ok || bad "$codec $lv $f: test failed"
			"$C" -q -d -o "$T/$f.out" "$T/$f.$X" && same "$T/$f" "$T/$f.out" "$codec $lv -d -o"
		done
		# pipes, small blocks, no checksum
		"$C" -q -c -B10 --no-check < "$T/$f" | "$C" -q -d -c > "$T/$f.pipe" && same "$T/$f" "$T/$f.pipe" "$codec pipe -B10"
	done
done

# WLZ4's fast and lazy modes, and WZIP data decoded by wlz4 (decompression reads either codec)
"$WLZ4" -q -f --fast "$T/big3m" && "$WZIP" -q -d -c "$T/big3m.wlz4" > "$T/f.out" && same "$T/big3m" "$T/f.out" "--fast"
"$WZIP" -q -f -5 "$T/big3m" && "$WLZ4" -q -d -c "$T/big3m.wz" > "$T/f.out" && same "$T/big3m" "$T/f.out" "wlz4 -d of .wz"

# threads: up to 7 (WZIP levels 7-13) give the frame of one thread, from a file, from a pipe and with small blocks
# (-B16); more cut the content of a pipe (its size unknown) into linked blocks, the same for any such count, which -l
# shows by their window log; -d needs no option for any of them
for src in file pipe; do
	for opt in "" -B16; do
		for t in 1 3 7 16; do
			if [ $src = file ]; then "$WZIP" -q -c -7 $opt -T$t "$T/big3m" > "$T/t$t.wz"
			else "$WZIP" -q -c -7 $opt -T$t < "$T/big3m" > "$T/t$t.wz"; fi
			"$WZIP" -q -d -c "$T/t$t.wz" > "$T/t.out" && same "$T/big3m" "$T/t.out" "$src $opt -T$t -d"
		done
		same "$T/t1.wz" "$T/t3.wz" "$src $opt -T3"; same "$T/t1.wz" "$T/t7.wz" "$src $opt -T7"
	done
done
"$WZIP" -q -c -7 -T8 < "$T/big3m" > "$T/t8.wz"; "$WZIP" -q -c -7 -T16 < "$T/big3m" > "$T/t16.wz"
same "$T/t8.wz" "$T/t16.wz" "pipe -T8"
"$WZIP" -l "$T/t16.wz" | grep -q " 26  21 " && ok || bad "-l of linked blocks (level 7: window 2^21)"
"$WZIP" -q -c -3 -T2 "$T/big3m" | "$WZIP" -l - | grep -q " 25   - " && ok || bad "-l of one block (-T2, 21 MB)"
"$WZIP" -q -c -3 < "$T/big3m" | "$WZIP" -l - | grep -q " 27   - " && ok || bad "-l of a pipe, one thread"

# concatenated frames of both codecs, with a skippable frame between them
"$WZIP" -q -c "$T/text8" > "$T/cat.wz"
printf 'P*M\030\003\000\000\000abc' >> "$T/cat.wz"
"$WLZ4" -q -c "$T/hex" >> "$T/cat.wz"
cat "$T/text8" "$T/hex" > "$T/cat.ref"
"$WZIP" -q -d -c "$T/cat.wz" > "$T/cat.out" && same "$T/cat.ref" "$T/cat.out" "concatenated frames"
"$WZIP" -l "$T/cat.wz" | grep -q mixed && ok || bad "-l on mixed frames"

# --rm removes the input, -k keeps it, an existing output is not overwritten without -f
cp "$T/text" "$T/rmme"
"$WZIP" -q --rm "$T/rmme" && [ ! -f "$T/rmme" ] && [ -f "$T/rmme.wz" ] && ok || bad "--rm"
"$WZIP" -q -d "$T/rmme.wz" && same "$T/text" "$T/rmme" "-d restores the name"
"$WZIP" -q "$T/rmme" 2>/dev/null && bad "overwrote an existing .wz without -f" || ok
"$WZIP" -q -f "$T/rmme" && ok || bad "-f did not overwrite"

# errors: unknown suffix, not a WZ file, damaged and truncated files, trailing garbage
"$WZIP" -q -d "$T/text" 2>/dev/null && bad "accepted an unknown suffix" || ok
"$WZIP" -q -d -c < "$T/text" > /dev/null 2>&1 && bad "decoded a file that is not WZ" || ok
"$WZIP" -q -c "$T/text8" > "$T/d.wz"
size=$(wc -c < "$T/d.wz")
head -c $((size - 1)) "$T/d.wz" > "$T/trunc.wz"
"$WZIP" -q -t "$T/trunc.wz" 2>/dev/null && bad "accepted a truncated file" || ok
# flip one byte in the middle of the compressed data
half=$((size / 2))
{ head -c $half "$T/d.wz"; printf '\377'; tail -c +$((half + 2)) "$T/d.wz"; } > "$T/flip.wz"
cmp -s "$T/d.wz" "$T/flip.wz" && printf '\376' | dd of="$T/flip.wz" bs=1 seek=$half conv=notrunc 2>/dev/null
"$WZIP" -q -t "$T/flip.wz" 2>/dev/null && bad "accepted a damaged file" || ok
{ cat "$T/d.wz"; printf 'garbage'; } > "$T/garb.wz"
"$WZIP" -q -t "$T/garb.wz" 2>/dev/null && bad "accepted trailing garbage" || ok

# the benchmark mode runs
"$WLZ4" -b2 "$T/text8" > /dev/null && ok || bad "-b"

echo "$checks checks, $fails failures (command-line tool)"
[ $fails -eq 0 ]
