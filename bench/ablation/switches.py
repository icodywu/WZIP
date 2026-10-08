"""Adds the ablation switches of the DCC paper's Section 6 to a copy of WZIP_L.c (never to src/). Each is an
environment variable read by the encoder; the decoder reads the stream as usual.
   WZ_FULLWIN=1  every level searches the input's full windows (2^27 at most) instead of its own level window, as all
                 levels did before the level windows; the paper's ablation runs set it, so that levels 11, 12 and 13
                 differ only in their parser (12: three states; 13: a first pass and eight offset groups)
   WZ_CLASSIC=1  code every sequence block with the 204-value alphabet (literal-run class x length), the cache slot
                 going to the offset symbol, instead of choosing the 1020-value joint alphabet per block; the parse
                 is unchanged, only its coding differs
   WZ_NG=k       k offset tables, for lengths 3, 4, ..., k+1 and k+2+ (k=1: one table for all lengths); the parser
                 prices offsets with these tables, so the parse changes too
   WZ_ONEWIN=1   lengths 3-7 get the widest window (no length-dependent windows); offsets are still coded per group,
                 and WZ_NG must be set so that the decoder, which derives the groups from the windows, agrees
usage: python switches.py path/to/WZIP_L.c"""
import sys

p = sys.argv[1]
s = open(p, newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
s = s.replace('\r\n', '\n')


def rep(old, new):
    global s
    if s.count(old) != 1:
        sys.exit('pattern count %d: %r' % (s.count(old), old[:80]))
    s = s.replace(old, new)


rep('static void Set_Offset_Groups(WZL_Sched* const S_, int natural, int fine)\n{\n',
    'static void Set_Offset_Groups(WZL_Sched* const S_, int natural, int fine)\n{\n'
    '\tif (getenv("WZ_NG") && !fine) natural = atoi(getenv("WZ_NG"));\n')
rep('\tconst int slotJoint = sjBits + (clBits >> 6) < clBits;\n',
    '\tconst int slotJoint = getenv("WZ_CLASSIC") ? 0 : sjBits + (clBits >> 6) < clBits;\n')
rep('\tfor (int k = 4; k <= 8; k++)\n\t\tOffWidth[k] = max(OffWidth[k], OffWidth[k - 1]);\n',
    '\tfor (int k = 4; k <= 8; k++)\n\t\tOffWidth[k] = max(OffWidth[k], OffWidth[k - 1]);\n'
    '\tif (getenv("WZ_ONEWIN")) for (int k = 3; k <= 7; k++) OffWidth[k] = OffWidth[8];\n')
rep('\t\tint prev = WZIP_LEVEL_WINDOW_LOG(level) + 1;',
    '\t\tint prev = (getenv("WZ_FULLWIN") ? 27 : WZIP_LEVEL_WINDOW_LOG(level)) + 1;')
open(p, 'w', newline='').write(s.replace('\n', nl))
print('switches added to', p)
