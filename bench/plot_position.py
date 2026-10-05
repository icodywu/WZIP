"""Where WLZ4 sits between LZ4 and Zstandard: ratio vs decompression speed and ratio vs compression speed on Silesia,
   from run_position.sh's results; prints the README table (Markdown) on standard output.
   usage: plot_position.py <out.svg> <results>... [--dec <decompression-only results>...] [--checked <results>...]
   A configuration measured in several passes keeps its best speeds; the files after --dec replace the decompression
   speeds of their configurations, and those after --checked give WLZ4's bounds-checked decompression speeds. Arrows
   join two pairs of equal ratio (WLZ4 lazy and Zstandard -1, WLZ4 12 and Zstandard 3), labeled with the speed ratio."""
import re, sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

def load(paths):
    d = {}
    for path in paths:
        for line in open(path):
            m = re.match(r'(\S+)\s+(-?\d+)\s+ratio ([\d.]+)\s+comp\s+([\d.]+) MB/s\s+dec\s+([\d.]+) MB/s', line)
            if m:
                k = (m.group(1), int(m.group(2)))
                v = (float(m.group(3)), float(m.group(4)), float(m.group(5)))
                if k in d:                               # several passes: the best speeds
                    v = (v[0], max(v[1], d[k][1]), max(v[2], d[k][2]))
                d[k] = v
    return d

out = sys.argv[1]
groups, cur = {'': [], '--dec': [], '--checked': []}, ''
for a in sys.argv[2:]:
    if a in groups: cur = a
    else: groups[cur].append(a)
d = load(groups[''])
for k, v in load(groups['--dec']).items():
    if k in d: d[k] = (d[k][0], d[k][1], max(d[k][2], v[2]))
checked = {k: v[2] for k, v in load(groups['--checked']).items()}

def order(c, l):                                         # level order within a family
    return {'lz4': 0, 'lz4hc': 1, 'wlz4f': 0, 'wlz4l': 1, 'wlz4hc': 2}.get(c, 0) * 1000 + l

def name(c, l):
    return {'lz4': 'LZ4 %d' % l, 'lz4hc': 'LZ4HC %d' % l, 'wlz4f': 'WLZ4 fast', 'wlz4l': 'WLZ4 lazy',
            'wlz4hc': 'WLZ4 %d' % l}.get(c, 'Zstandard %d' % l)

def label(c, l):                                         # the point labels: levels only (WLZ4 4 unlabeled: crowded)
    return '' if (c, l) == ('wlz4hc', 4) else {'wlz4f': 'f', 'wlz4l': 'l'}.get(c, str(l))

families = [
    (['lz4', 'lz4hc'], 'LZ4 / LZ4HC 1.10', 'o', '#6b6b6b', '--'),
    (['wlz4f', 'wlz4l', 'wlz4hc'], 'WLZ4', 's', '#c0392b', '-'),
    (['zstd'], 'Zstandard 1.5.6', '^', '#1f5fa8', '--'),
]
pairs = [(('wlz4l', 0), ('zstd', -1)), (('wlz4hc', 12), ('zstd', 3))]   # equal ratios: arrows from the slower one
plt.rcParams.update({'font.size': 9, 'svg.fonttype': 'none', 'font.family': 'sans-serif'})
fig, axes = plt.subplots(1, 2, figsize=(7.6, 3.2), facecolor='white')
handles = []
for ax, col, xlabel in ((axes[0], 2, 'Decompression speed (MB/s)'), (axes[1], 1, 'Compression speed (MB/s, log scale)')):
    ax.set_facecolor('white')
    for names, lab, mk, color, ls in families:
        pts = sorted([(order(c, l), (c, l), v) for (c, l), v in d.items() if c in names])
        if not pts: continue
        xs, ys = [v[col] for _, _, v in pts], [v[0] for _, _, v in pts]
        h, = ax.plot(xs, ys, marker=mk, color=color, linestyle=ls, label=lab, markersize=4.5, linewidth=1.2)
        if col == 2: handles.append(h)
        for (_, (c, l), v) in pts:
            ax.annotate(label(c, l), (v[col], v[0]), textcoords='offset points', xytext=(4, -3), fontsize=7,
                        color=color)
        if col == 2 and names[0] == 'wlz4f' and checked:
            cp = [(v[0], checked[k]) for _, k, v in pts if k in checked]
            h, = ax.plot([x for _, x in cp], [y for y, _ in cp], marker=mk, color=color, linestyle=':',
                         markerfacecolor='white', label='WLZ4, checked decoder', markersize=4.5, linewidth=1.0)
            handles.append(h)
    for a, b in pairs:
        if a not in d or b not in d: continue
        lo, hi = sorted((d[a], d[b]), key=lambda v: v[col])
        ax.annotate('', (hi[col], hi[0]), (lo[col], lo[0]), arrowprops=dict(arrowstyle='->', color='#888888',
                    lw=0.9, shrinkA=5, shrinkB=5))
        mid = (lo[col] * hi[col]) ** 0.5 if col == 1 else (lo[col] + hi[col]) / 2
        ax.annotate('%.1f×' % (hi[col] / lo[col]) if hi[col] / lo[col] < 10 else '%.0f×' % (hi[col] / lo[col]),
                    (mid, (lo[0] + hi[0]) / 2), textcoords='offset points', xytext=(0, 4), ha='center',
                    fontsize=7.5, color='#555555')
    if col == 1: ax.set_xscale('log')
    ax.set_xlabel(xlabel)
    ax.grid(True, which='major', alpha=0.3)
axes[0].set_ylabel('Compression ratio')
axes[0].set_title('Decompression: WLZ4 between the two', fontsize=9)
axes[1].set_title('Compression: Zstandard faster at equal ratio', fontsize=9)
fig.legend(handles=handles, loc='lower center', ncol=4, fontsize=8.5, frameon=False, bbox_to_anchor=(0.5, -0.01))
plt.tight_layout(rect=(0, 0.07, 1, 1))
plt.savefig(out, facecolor='white', metadata={'Date': None})
print('wrote', out, file=sys.stderr)

print('| Codec, level | Ratio | Compression (MB/s) | Decompression (MB/s) |')
print('|---|---:|---:|---:|')
for names, _, _, _, _ in families:
    for k in sorted([k for k in d if k[0] in names], key=lambda k: order(*k)):
        r, cs, ds = d[k]
        dec = '%.0f' % ds + (' (checked %.0f)' % checked[k] if k in checked else '')
        print('| %s | %.3f | %s | %s |' % (name(*k), r, ('%.1f' % cs) if cs < 100 else '%.0f' % cs, dec))
