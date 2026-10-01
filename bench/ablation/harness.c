/* One file at one WZIP level: compress, decompress (checked mode), verify; prints sizes, ratio, compression speed and
   the process's peak working set (Windows) or maximum resident set (elsewhere). Used by run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "WZIP.h"
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
static double now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart; }
static double peak_mb(void) { PROCESS_MEMORY_COUNTERS pmc; GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)); return pmc.PeakWorkingSetSize / 1e6; }
#else
#include <time.h>
#include <sys/resource.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static double peak_mb(void) { struct rusage r; getrusage(RUSAGE_SELF, &r); return r.ru_maxrss / 1e3; }
#endif

int main(int argc, char** argv)
{
	if (argc < 3) { fprintf(stderr, "usage: %s level file\n", argv[0]); return 2; }
	const int level = atoi(argv[1]);
	FILE* fp = fopen(argv[2], "rb");
	if (!fp) { fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }
	fseek(fp, 0, SEEK_END); const long n = ftell(fp); fseek(fp, 0, SEEK_SET);
	unsigned char* src = malloc(n);
	if (fread(src, 1, n, fp) != (size_t)n) { fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }
	fclose(fp);
	int cap = WZIP_Cap_CmprSize((int)n) + 1024;
	unsigned char* cmp = malloc(cap);
	const double t0 = now();
	const int c = wzip_compress(src, (int)n, cmp, &cap, level);
	const double t1 = now();
	const double peak = peak_mb();                    /* before the decoder's buffer: the encoder's peak */
	int dcap = (int)n + WZIP_MEM_OVERHEAD;
	unsigned char* dec = malloc(dcap);
	const int d = wzip_decompress(cmp, c, dec, &dcap);
	const int ok = d == n && !memcmp(src, dec, n);
	printf("L%d %s %ld -> %d ratio %.4f comp %.2f MB/s peak %.0f MB %s\n", level, argv[2], n, c, (double)n / c,
	       n / 1e6 / (t1 - t0), peak, ok ? "OK" : "FAIL");
	return !ok;
}
