/* Match finding in threads (WZIP_Set_Workers): mt_find <level> <maxWorkers> [dictKB] file...
   Compresses each file with WZIP_L at 1..maxWorkers workers (optionally with the file's first dictKB KiB as a prefix
   dictionary), checks that every output equals the single-threaded one and decodes, and prints the wall time.
   Build (Linux): gcc -O2 -I../src -DWZIP_MULTITHREAD=1 -pthread mt_find.c ../src/*.c -lm -o mt_find
   results/epyc_threads.txt: each run on one 8-core complex (taskset -c 0-7, 8-15, ...), six at once per node. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "WZIP.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char** argv)
{
	const int level = atoi(argv[1]), maxW = atoi(argv[2]), dictKB = atoi(argv[3]);
	double t[17] = { 0 };
	size_t total = 0, csum = 0;
	int same = 1, decoded = 1;
	for (int a = 4; a < argc; a++) {
		FILE* f = fopen(argv[a], "rb");
		if (!f) return 1;
		fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
		unsigned char* s = (unsigned char*)malloc(n);
		if (fread(s, 1, n, f) != (size_t)n) return 1;
		fclose(f);
		const int dn = (int)((long)dictKB * 1024 < n / 2 ? (long)dictKB * 1024 : 0), len = (int)(n - dn);
		if (len < 32768) { free(s); continue; }
		const int cap = len + 4096;
		unsigned char* ref = (unsigned char*)malloc(cap), *c = (unsigned char*)malloc(cap), *d = (unsigned char*)malloc(len + 64);
		int refSize = 0;
		for (int w = 1; w <= maxW; w++) {
			WZIP_State_Str* st = WZIP_New_State_L(level, len, dn ? s : NULL, dn);
			WZIP_Set_Workers(st, w);
			const double t0 = now();
			const int cs = WZIP_Compress_L(st, s + dn, len, w == 1 ? ref : c, cap);
			t[w] += now() - t0;
			WZIP_Free_State(st);
			if (w == 1) {
				refSize = cs;
				csum += cs;
				if (cs > 0 && (WZIP_Decompress_L(ref, cs, d, len, dn ? s : NULL, dn) != len || memcmp(d, s + dn, len))) decoded = 0;
			}
			else if (cs != refSize || memcmp(c, ref, cs)) { same = 0; fprintf(stderr, "%s: %d workers differ\n", argv[a], w); }
		}
		total += len;
		free(s); free(ref); free(c); free(d);
	}
	printf("level %d dict %d KB: %zu -> %zu (%.4f), outputs %s, decoded %s\n", level, dictKB, total, csum, (double)total / csum,
	       same ? "identical" : "DIFFER", decoded ? "ok" : "FAILED");
	for (int w = 1; w <= maxW; w++)
		printf("  workers %d: %.1f s, %.2f MB/s, x%.2f\n", w, t[w], total / 1e6 / t[w], t[1] / t[w]);
	return !(same && decoded);
}
