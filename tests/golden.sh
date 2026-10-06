#!/bin/sh
# Golden frames: every frame in tests/golden, written by an earlier version, must still decode to its input, with the
# tool and (if python is present) with the reference decoder written from the specifications.
# usage: sh tests/golden.sh <directory holding wzip>
B=$(cd "$1" && pwd); EXE=; [ -f "$B/wzip.exe" ] && EXE=.exe
D=$(cd "$(dirname "$0")" && pwd)
G="$D/golden"
PY=$(command -v python3 || command -v python)
T=$(mktemp -d 2>/dev/null || echo "${TMPDIR:-/tmp}/wzip-golden-$$"); mkdir -p "$T"; trap 'rm -rf "$T"' EXIT
fails=0; n=0
for f in "$G"/*.wz "$G"/*.wlz4; do
	name=$(basename "$f")
	case $name in
		concat.*) ref=concat.ref ;;
		*) ref=$(echo "$name" | sed -E 's/\.(L[0-9]*|lazy|fast)\..*$//') ;;
	esac
	n=$((n + 1))
	if "$B/wzip$EXE" -q -d -c "$f" > "$T/out" && cmp -s "$T/out" "$G/$ref"; then :; else echo "FAIL (tool): $name"; fails=$((fails + 1)); fi
	if [ -n "$PY" ]; then
		if (cd "$D/../doc" && "$PY" -B frame_decode.py "$f" "$T/pyout") && cmp -s "$T/pyout" "$G/$ref"; then :
		else echo "FAIL (reference decoder): $name"; fails=$((fails + 1)); fi
	fi
done
echo "$n golden frames, $fails failures${PY:+ (tool and reference decoder)}"
[ $fails -eq 0 ]
