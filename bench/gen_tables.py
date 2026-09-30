"""LaTeX tables of the paper from bench_all output.
   usage: gen_tables.py <outdir> S=<file>[,<file>...] C=<file>[,<file>...]
   A configuration measured more than once (several passes) keeps its best speeds; ratio and memory are the same."""
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

outdir = sys.argv[1]
args = dict(a.split('=', 1) for a in sys.argv[2:])
S, C = load(args['S']), load(args['C'])
E8 = load(args['E8']) if 'E8' in args else {}
E9 = load(args['E9']) if 'E9' in args else {}

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
           ' & \\multicolumn{3}{c}{%s} & \\multicolumn{3}{c}{%s} & Enc. \\\\' % names,
           '\\cmidrule(lr){2-4}\\cmidrule(lr){5-7}',
           'Codec, level & Ratio & Comp. & Dec. & Ratio & Comp. & Dec. & mem. \\\\', '\\midrule']
    for gi, g in enumerate(groups):
        if gi: out.append('\\midrule')
        for c, l in g:
            r = row(c, l, S, C)
            if r: out.append(r)
    out += ['\\bottomrule', '\\end{tabular}}', '\\end{center}', '\\end{table}']
    return '\n'.join(out) + '\n'

cap = 'ratio, compression and decompression speed (MB/s), and encoder memory on Silesia (MB).'
open(os.path.join(outdir, 'tab_lz4class.tex'), 'w').write(table('The LZ4 class: ' + cap, 'tab:lz4class',
    [[('lz4', 1), ('lz4hc', 4), ('lz4hc', 9), ('lz4hc', 12)],
     [('wlz4f', 1), ('wlz4l', 0), ('wlz4hc', 2), ('wlz4hc', 4), ('wlz4hc', 6), ('wlz4hc', 8), ('wlz4hc', 10), ('wlz4hc', 12)]]))
open(os.path.join(outdir, 'tab_zstdclass.tex'), 'w').write(table('The Zstandard class: ' + cap, 'tab:zstdclass',
    [[('zstd', l) for l in (1, 3, 9, 12, 16, 19, 22)],
     [('wzip', l) for l in (0, 1, 3, 5, 7, 9, 11, 12, 13)],
     [('brotli', l) for l in (1, 5, 9, 11)] + [('xz', l) for l in (1, 6, 109)]]))
if E8 and E9:
    memSecond = True
    open(os.path.join(outdir, 'tab_enwik.tex'), 'w').write(table(
        'Large inputs: ratio, compression and decompression speed (MB/s) on enwik8 (100\\,MB) and enwik9 (1\\,GB), '
        'and encoder memory on enwik9 (MB).', 'tab:enwik',
        [[('lz4', 1), ('lz4hc', 9), ('lz4hc', 12)],
         [('wlz4f', 1), ('wlz4hc', 2), ('wlz4hc', 6), ('wlz4hc', 10), ('wlz4hc', 12)],
         [('zstd', l) for l in (1, 3, 19, 22)],
         [('wzip', l) for l in (0, 3, 9, 11, 13)],
         [('brotli', 11), ('xz', 109)]], E8, E9, ('enwik8', 'enwik9'), '!htb'))
print('wrote tables to', outdir)
