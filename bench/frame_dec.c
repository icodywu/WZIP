/* Decoding speed of WZ files in memory (WZF_decompress), best of N runs: frame_dec N file...
   Build (Linux): gcc -O2 -I../src frame_dec.c ../src/*.c -lm -o frame_dec; run pinned, e.g. taskset -c 4 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "wzframe.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char** argv)
{
	const int runs = atoi(argv[1]);
	for (int i = 2; i < argc; i++) {
		FILE* f = fopen(argv[i], "rb");
		fseek(f, 0, SEEK_END);
		const size_t n = (size_t)ftell(f);
		fseek(f, 0, SEEK_SET);
		unsigned char* s = malloc(n);
		if (fread(s, 1, n, f) != n) return 1;
		fclose(f);
		const unsigned long long cs = WZF_getContentSize(s, n);
		unsigned char* d = malloc(cs);
		memset(d, 1, cs);
		double best = 1e30;
		for (int r = 0; r < runs; r++) {
			const double t0 = now();
			const size_t got = WZF_decompress(d, cs, s, n);
			const double t = now() - t0;
			if (got != cs) { printf("%s: %s\n", argv[i], WZF_isError(got) ? WZF_getErrorName(got) : "short"); return 1; }
			if (t < best) best = t;
		}
		printf("%-40s %12zu -> %12llu  %8.1f MB/s\n", argv[i], n, cs, cs / best / 1e6);
		free(s); free(d);
	}
	return 0;
}
