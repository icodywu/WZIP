#!/bin/sh
# One exclusive node of a cluster benchmark run: eight workers, one core in each core complex (CCD) of the two AMD EPYC
# 9334 sockets, the other cores idle, so that no two runs share an L3 cache. Submit as an array, one task per node:
#   sbatch --array=0-15 --export=ALL,RUN=/path/to/run bench/cluster/node.sbatch       (from the repository root)
#SBATCH -p hermes-2
#SBATCH --exclusive
#SBATCH --time=04:00:00
#SBATCH -J wzbench
#SBATCH -o %x-%A_%a.out
cd "$SLURM_SUBMIT_DIR/bench"
lscpu | grep -E 'Model name|MHz' > "$RUN/nodes/$(hostname).txt"
for cpu in 4 12 20 28 36 44 52 60; do
	sh cluster/worker.sh "$RUN" $cpu &
done
wait
