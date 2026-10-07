"""LaTeX tables of the paper from bench_all output.
   usage: gen_tables.py <outdir> S=<file>[,<file>...] C=<file>[,<file>...] [E8=... E9=...] [SD=... CD=... E8D=... E9D=...]
                        [SX=... CX=... E8X=... E9X=...]
   A configuration measured more than once (several passes) keeps its best speeds; ratio and memory are the same.
   SD, CD, E8D, E9D: decompression-only runs (bench_all with STREAMS); their decompression speeds replace those of
   the same configurations, whose other columns stay (the ratio is checked to match).
   SX, CX, E8X, E9X: replacement runs: every codec they contain loses all its entries from the other files first
   (used when a codec's format changed, so that earlier measurements of that codec are left out)."""
import re, sys, os

def load(paths):
    d = {}
    for path in paths.split(','):
        for line in open(path):
            m = re.match(r'(\S+)\s+(\d+)\s+ratio ([\d.]+)\s+comp\s+([\d.]+) MB/s\s+dec\s+([\d.]+) MB/s\s+encmem\s+([\d.]+) MB', line)
            if m:
                k = (m.group(1), int(m.group(2)))
                v = (float(m.group(3)), float(m.group(4)), float(m.group(5)), float(m.group(6)))
                if k in d:
                    o = d[k]
                    v = (v[0], max(v[1], o[1]), max(v[2], o[2]), max(v[3], o[3]))
                d[k] = v
    return d

def override_dec(d, paths, tag):
    """replaces the decompression speeds of d with those of decompression-only runs"""
    for k, v in load(paths).items():
        if k not in d:
            print('%s: %s %d has no full measurement; skipped' % (tag, k[0], k[1]))
            continue
        if abs(d[k][0] - v[0]) > 5e-5:
            print('%s: %s %d ratio %.4f differs from %.4f' % (tag, k[0], k[1], v[0], d[k][0]))
        d[k] = (d[k][0], d[k][1], v[2], d[k][3])

def replace_codecs(d, paths):
    """drops every entry of the codecs in the replacement runs, then adds those runs"""
    r = load(paths)
    codecs = {k[0] for k in r}
    for k in [k for k in d if k[0] in codecs]:
        del d[k]
    d.update(r)

outdir = sys.argv[1]
args = dict(a.split('=', 1) for a in sys.argv[2:])
S, C = load(args['S']), load(args['C'])
E8 = load(args['E8']) if 'E8' in args else {}
E9 = load(args['E9']) if 'E9' in args else {}
for tag, d in (('SX', S), ('CX', C), ('E8X', E8), ('E9X', E9)):
    if tag in args: replace_codecs(d, args[tag])
for tag, d in (('SD', S), ('CD', C), ('E8D', E8), ('E9D', E9)):
    if tag in args: override_dec(d, args[tag], tag)

def label(c, l):
    if c == 'lz4': return 'LZ4 (default)'
    if c == 'lz4hc': return 'LZ4HC %d' % l
    if c == 'wlz4f': return 'WLZ4 fast'
    if c == 'wlz4l': return 'WLZ4 lazy'
    if c == 'wlz4hc': return 'WLZ4 %d' % l
    if c == 'zstd': return 'Zstandard %d' % l
    if c == 'wzip': return 'WZIP %d' % l
    if c == 'brotli': return 'Brotli %d' % l
    if c == 'xz': return 'xz -9e' if l == 109 else 'xz -%d' % l
    return '%s %d' % (c, l)

def sp(v):
    if v < 0.01: return '$<$0.01'
    return '%.0f' % v if v >= 100 else ('%.1f' % v if v >= 10 else '%.2f' % v)

def mem(v):
    return '%.1f' % v if v < 10 else '%.0f' % v

def row(c, l, S=S, C=C):
    s, cc = S.get((c, l)), C.get((c, l))
    if not s: return None
    memv = (cc if memSecond and cc else s)[3]
    cells = [label(c, l), '%.3f' % s[0], sp(s[1]), '%.0f' % s[2],
             '%.3f' % cc[0] if cc else '---', sp(cc[1]) if cc else '---', '%.0f' % cc[2] if cc else '---', mem(memv)]
    if c in ('wlz4f', 'wlz4l', 'wlz4hc', 'wzip'):
        cells[0] = '\\textbf{' + cells[0] + '}'
    return ' & '.join(cells) + ' \\\\'

memSecond = False

def table(caption, lab, groups, S=S, C=C, names=('Silesia', 'Canterbury+Calgary'), place='t'):
    out = ['\\begin{table}[%s]' % place, '\\begin{center}', '\\caption{\\label{%s}%s}' % (lab, caption), '{\\footnotesize',
           '\\setlength{\\tabcolsep}{4.5pt}',
           '\\begin{tabular}{lrrrrrrr}', '\\toprule',
           ' & \\multicolumn{3}{c}{%s} & \\multicolumn{3}{c}{%s} & Memory \\\\' % names,
           '\\cmidrule(lr){2-4}\\cmidrule(lr){5-7}',
           'Codec, level & Ratio & Comp. & Dec. & Ratio & Comp. & Dec. & (MB) \\\\', '\\midrule']
    for gi, g in enumerate(groups):
        if gi: out.append('\\midrule')
        for c, l in g:
            r = row(c, l, S, C)
            if r: out.append(r)
    out += ['\\bottomrule', '\\end{tabular}}', '\\end{center}', '\\end{table}']
    return '\n'.join(out) + '\n'

def cap(first, second, memOn):
    return ('ratio, and compression and decompression speed (MB/s), on %s and on %s; encoder memory (MB) on %s.'
            % (first, second, memOn))
open(os.path.join(outdir, 'tab_lz4class.tex'), 'w').write(table(
    'The LZ4 class: ' + cap('Silesia', 'Canterbury+Calgary', 'Silesia'), 'tab:lz4class',
    [[('lz4', 1), ('lz4hc', 9), ('lz4hc', 12)],
     [('wlz4f', 1), ('wlz4l', 0), ('wlz4hc', 2), ('wlz4hc', 6), ('wlz4hc', 8), ('wlz4hc', 10), ('wlz4hc', 12)]]))
open(os.path.join(outdir, 'tab_zstdclass.tex'), 'w').write(table(
    'The Zstandard class: ' + cap('Silesia', 'Canterbury+Calgary', 'Silesia'), 'tab:zstdclass',
    [[('zstd', l) for l in (1, 3, 19, 22)],
     [('wzip', l) for l in (0, 3, 9, 11, 12)],
     [('brotli', 11)] + [('xz', l) for l in (6, 109)]]))
if E8 and E9:
    memSecond = True
    open(os.path.join(outdir, 'tab_enwik.tex'), 'w').write(table(
        'Large inputs: ' + cap('enwik8 (100\\,MB)', 'enwik9 (1\\,GB)', 'enwik9'), 'tab:enwik',
        [[('lz4', 1), ('lz4hc', 12)],
         [('wlz4hc', 6), ('wlz4hc', 10), ('wlz4hc', 12)],
         [('zstd', l) for l in (19, 22)],
         [('wzip', l) for l in (11, 12, 13)],
         [('brotli', 11), ('xz', 109)]], E8, E9, ('enwik8', 'enwik9'), '!htb'))
print('wrote tables to', outdir)
