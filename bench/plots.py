"""Ratio vs compression speed and ratio vs decompression speed on Silesia, from bench_all output files
   (later files override earlier ones). usage: plots.py <out.pdf> <results>..."""
import re, sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

out = sys.argv[1]
d = {}
for path in sys.argv[2:]:
    for line in open(path):
        m = re.match(r'(\S+)\s+(\d+)\s+ratio ([\d.]+)\s+comp\s+([\d.]+) MB/s\s+dec\s+([\d.]+) MB/s', line)
        if m:
            k = (m.group(1), int(m.group(2)))
            v = (float(m.group(3)), float(m.group(4)), float(m.group(5)))
            if k in d:                                   # several passes: the best speeds
                v = (v[0], max(v[1], d[k][1]), max(v[2], d[k][2]))
            d[k] = v

# each family's points in level order (fast/lazy WLZ4 before its hash-chain levels)
def order(c, l):
    return {'lz4': 0, 'lz4hc': 1, 'wlz4f': 0, 'wlz4l': 1, 'wlz4hc': 2}.get(c, 0) * 1000 + l

families = [
    (['lz4', 'lz4hc'], 'LZ4/LZ4HC 1.10', 'o', 'tab:gray', '--'),
    (['wlz4f', 'wlz4l', 'wlz4hc'], 'WLZ4', 's', 'tab:red', '-'),
    (['zstd'], 'Zstandard 1.5.6', '^', 'tab:blue', '--'),
    (['wzip'], 'WZIP', 'P', 'tab:orange', '-'),
    (['brotli'], 'Brotli 1.1.0', 'v', 'tab:green', ':'),
    (['xz'], 'xz 5.6.2', 'D', 'tab:purple', ':'),
]
plt.rcParams.update({'font.size': 9})
fig, axes = plt.subplots(1, 2, figsize=(6.0, 2.6))
handles = []
for ax, col, xlabel in ((axes[0], 1, 'Compression speed (MB/s)'), (axes[1], 2, 'Decompression speed (MB/s)')):
    for names, label, mk, color, ls in families:
        pts = sorted([(order(c, l), v) for (c, l), v in d.items() if c in names])
        if not pts: continue
        h, = ax.plot([v[col] for _, v in pts], [v[0] for _, v in pts], marker=mk, color=color, linestyle=ls,
                     label=label, markersize=4, linewidth=1.1)
        if col == 1: handles.append(h)
    ax.set_xscale('log')
    ax.set_xlabel(xlabel)
    ax.grid(True, which='both', alpha=0.25)
axes[0].set_ylabel('Compression ratio')
fig.legend(handles=handles, loc='lower center', ncol=6, fontsize=8.5, columnspacing=1.0, handletextpad=0.4, frameon=False, bbox_to_anchor=(0.5, -0.01))
plt.tight_layout(rect=(0, 0.09, 1, 1))
plt.savefig(out)
print('wrote', out)
