"""Collects a cluster benchmark run into one result file per corpus, in bench_all's format.

    python3 collect.py <run directory> <output prefix>     e.g.  collect.py ~/wzbench/run1 ../results/epyc

writes <prefix>_S.txt, <prefix>_C.txt, <prefix>_e8.txt, <prefix>_e9.txt (codecs and levels in the runlists' order),
<prefix>_checked_S.txt (tasks with the checked decoders), and <prefix>_log.txt (node, core and seconds of each task);
for a block benchmark run (make_queue.py blocks), <prefix>_blocks_S.txt and <prefix>_blocks_C.txt in the queue's
order; reports missing or failed tasks."""
import os
import re
import sys

run, prefix = sys.argv[1], sys.argv[2]
names = {'S': 'S', 'C': 'C', 'E8': 'e8', 'E9': 'e9', 'S.checked': 'checked_S'}
order = {}
for path in ('runlist.txt', 'runlist_position.txt', 'runlist_e9.txt'):
    for line in open(os.path.join(os.path.dirname(__file__), '..', path)):
        f = line.split()
        if len(f) == 3 and (f[0], int(f[1])) not in order:
            order[(f[0], int(f[1]))] = len(order)
lines, log, problems = {k: [] for k in names}, [], []
blocks = {}
for task in open(os.path.join(run, 'queue.txt')):
    f = task.split()
    if f[0] == 'B':                                         # B corpus block-size codec level rounds
        path = os.path.join(run, 'out', 'B.%s.%s.%s.%s.txt' % tuple(f[1:5]))
        result = open(path).read().splitlines() if os.path.exists(path) else []
        result = [l for l in result if re.match(r'\S+\s+-?\d+\s+block', l)]
        if not result or not result[0].rstrip().endswith('fails 0'):
            problems.append(('missing ' if not os.path.exists(path) else 'failed ') + task.strip())
        else:
            blocks.setdefault(f[1], []).append(result[0])
        continue
    corpus, codec, level = f[0], f[1], f[2]
    mode = f[4] if len(f) > 4 else ''
    path = os.path.join(run, 'out', '%s.%s.%s%s.txt' % (corpus, codec, level, '.' + mode if mode else ''))
    corpus = corpus + ('.' + mode if mode else '')
    if not os.path.exists(path):
        problems.append('missing ' + task.strip())
        continue
    text = open(path).read().splitlines()
    result = [l for l in text if re.match(r'\S+\s+-?\d+\s+ratio', l)]
    if not result or not result[0].rstrip().endswith('fails 0'):
        problems.append('failed ' + task.strip() + ': ' + ' | '.join(text[-2:]))
        continue
    lines[corpus].append((order.get((codec, int(level)), 999), result[0]))
    log.append(text[-1])
for corpus, l in blocks.items():
    with open('%s_blocks_%s.txt' % (prefix, corpus), 'w') as f:
        f.write('\n'.join(l) + '\n')
if not blocks:
    for corpus, name in names.items():
        with open('%s_%s.txt' % (prefix, name), 'w') as f:
            for _, l in sorted(lines[corpus]):
                f.write(l + '\n')
    with open(prefix + '_log.txt', 'w') as f:
        f.write('\n'.join(sorted(log)) + '\n')
print('%d results, %d problems' % (sum(len(v) for v in lines.values()) + sum(len(v) for v in blocks.values()),
                                   len(problems)))
for p in problems:
    print(p)
