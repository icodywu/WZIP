"""The task queue of a cluster benchmark run: one line per (corpus, codec, level, compression rounds), slowest first.

    python3 make_queue.py > queue.txt          (from bench/: reads runlist.txt, runlist_e9.txt, runlist_position.txt)
    python3 make_queue.py checked > ...        also WZIP's and WLZ4's levels on Silesia with their checked decoders
    python3 make_queue.py blocks > ...         the block benchmark (bench_blocks, 4 and 8 KB, Silesia and the small files)

Corpora: S Silesia, C Canterbury + Calgary, E8 enwik8, E9 enwik9 (runlist_e9.txt). Silesia also gets the levels of
runlist_position.txt (Zstandard's negative levels). The order is by a rough cost estimate, so that the longest tasks
start first and the run ends soon after its longest task."""

def runlist(name):
    out = []
    for line in open(name):
        f = line.split()
        if len(f) == 3:
            out.append((f[0], int(f[1]), int(f[2])))
    return out


main, e9, pos = runlist('runlist.txt'), runlist('runlist_e9.txt'), runlist('runlist_position.txt')
silesia = main + [t for t in pos if t not in main]
size = {'S': 212, 'C': 6, 'E8': 100, 'E9': 1000}           # MB
slow = {                                                    # rough seconds per MB at the slowest levels
    'wzip': lambda l: 2.5 if l >= 13 else 1.5 if l >= 11 else 0.6 if l >= 7 else 0.15 if l >= 3 else 0.02,
    'brotli': lambda l: 3.0 if l >= 11 else 0.2 if l >= 9 else 0.02,
    'xz': lambda l: 1.0 if l >= 6 else 0.3,
    'zstd': lambda l: 0.8 if l >= 19 else 0.3 if l >= 16 else 0.06 if l >= 12 else 0.01,
    'wlz4hc': lambda l: 1.2 if l >= 12 else 0.4 if l >= 10 else 0.2 if l >= 8 else 0.03,
    'lz4hc': lambda l: 0.15 if l >= 12 else 0.04,
}
tasks = []
for corpus, lst in (('S', silesia), ('C', main), ('E8', main), ('E9', e9)):
    for codec, level, rounds in lst:
        cost = size[corpus] * slow.get(codec, lambda l: 0.01)(level) * rounds + size[corpus] * 0.05
        tasks.append((cost, corpus, codec, level, rounds, ''))
import sys
if len(sys.argv) > 1 and sys.argv[1] == 'blocks':          # the block benchmark: B corpus size codec level rounds
    blocks = [('lz4', 1, 3), ('lz4hc', 9, 1), ('lz4hc', 12, 1), ('wlz4f', 1, 3), ('wlz4hc', 2, 1), ('wlz4hc', 10, 1),
              ('zstd', 1, 3), ('zstd', 3, 3), ('zstd', 9, 1), ('zstd', 19, 1), ('wzips', 1, 3), ('wzips', 5, 1),
              ('wzips', 9, 1), ('wzipm', 9, 1), ('wzipm', 12, 1)]
    for corpus in ('S', 'C'):
        for bs in (4096, 8192):
            for codec, level, rounds in blocks:
                print('B', corpus, bs, codec, level, rounds)
    sys.exit(0)
if len(sys.argv) > 1 and sys.argv[1] == 'checked':        # the bounds-checked decoders (bench_all CHECKED=1)
    for codec, level, rounds in silesia:
        if codec in ('wzip', 'wlz4f', 'wlz4l', 'wlz4hc'):
            cost = size['S'] * slow.get(codec, lambda l: 0.01)(level) + size['S'] * 0.05
            tasks.append((cost, 'S', codec, level, rounds, 'checked'))
for t in sorted(tasks, reverse=True):
    print(' '.join(str(x) for x in t[1:]))
