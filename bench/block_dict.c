/* One block of a linked WZ frame in isolation: block_dict file level offset blockSize dictSize workers...
   compresses file[offset, offset + blockSize) with the dictSize bytes before it (wzip_compress_usingDict), once per
   worker count, and prints the time. Build (Linux):
   gcc -O2 -I../src -DWZIP_MULTITHREAD=1 -pthread block_dict.c ../src/*.c -lm -o block_dict */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "WZIP.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char** argv)
{
	FILE* f = fopen(argv[1], "rb");
	const int level = atoi(argv[2]);
	const long off = atol(argv[3]), n = atol(argv[4]), dn = atol(argv[5]);
	unsigned char* buf = malloc(dn + n);
	fseek(f, off - dn, SEEK_SET);
	if (fread(buf, 1, dn + n, f) != (size_t)(dn + n)) return 1;
	fclose(f);
	int cap = (int)n + 1024;
	unsigned char* out = malloc(cap);
	for (int a = 6; a < argc; a++) {
		const int w = atoi(argv[a]);
		int c = cap;
		const double t0 = now();
		const int cs = wzip_compress_usingDict(buf + dn, (int)n, out, &c, level, w, dn ? buf : NULL, (int)dn);
		const double t = now() - t0;
		printf("L%d block %ld dict %ld workers %d: %d bytes, %.2f s, %.3f MB/s\n", level, n, dn, w, cs, t, n / t / 1e6);
		fflush(stdout);
	}
	return 0;
}
