"""Adds the ablation switches of the DCC paper's Section 5 to a copy of WZIP_L.c (never to src/). Each is an
environment variable read by the encoder; the decoder reads the stream as usual.
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


rep('static void Set_Offset_Groups(int natural, int fine)\n{\n',
    'static void Set_Offset_Groups(int natural, int fine)\n{\n\tif (getenv("WZ_NG") && !fine) natural = atoi(getenv("WZ_NG"));\n')
rep('\tconst int slotJoint = sjBits + (clBits >> 6) < clBits;\n',
    '\tconst int slotJoint = getenv("WZ_CLASSIC") ? 0 : sjBits + (clBits >> 6) < clBits;\n')
rep('\tSet_Offset_Groups(wzipStr.hash2Len - MinMatchLen + 1, OffGroupsFine);\n',
    '\tif (getenv("WZ_ONEWIN")) for (int k = 3; k <= 7; k++) OffWidth[k] = OffWidth[8];\n'
    '\tSet_Offset_Groups(wzipStr.hash2Len - MinMatchLen + 1, OffGroupsFine);\n')
open(p, 'w', newline='').write(s.replace('\n', nl))
print('switches added to', p)
