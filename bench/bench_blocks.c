/* Block benchmark: every file is cut into independent blocks of a fixed size (e.g. 4 KB storage pages), and each
   block is compressed and decompressed by its own call, as a page cache or a key-value store would. One thread pinned
   to core 2 at high priority, after a one-second warm-up. Ratio = total input / total output; speeds = total input /
   total time over all blocks, best of <rounds> passes for compression and of 10 for decompression; every block is
   verified.
   Usage: bench_blocks <codec> <level> <block size> <rounds> file...
   codecs: wzips (WZIP_S, 1-9), wzipm (WZIP_M through wzip_compress, 0-12), lz4 (acceleration), lz4hc (1-12),
           zstd (1-22), wlz4f (acceleration), wlz4hc (0-12) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <zstd.h>
#include <lz4.h>
#include <lz4hc.h>
#include "WZIP.h"
#include "WLZ4.h"

static double now(void) { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart / f.QuadPart; }

static const char* codec;
static int level;
static int checked;         /* WZIP_M and WLZ4 decode in their trusted mode (each slot has the slack); CHECKED=1: checked */
static ZSTD_CCtx* zc; static ZSTD_DCtx* zd;
static WZIPS_CCtx* sc;
static WLZ_State_Str* ws; static WLZhc_State_Str* hs;

enum { WZIPS, WZIPM, LZ4F, LZ4HC, ZSTD, WLZ4F, WLZ4HC };
static int kind;
static void* hcState;                                    /* LZ4HC's state, allocated once as for the other codecs */

static int compress1(const unsigned char* src, int n, unsigned char* dst, int cap)
{
	switch (kind) {
	case WZIPS:  return WZIPS_compress(sc, src, n, dst, cap, level);
	case WZIPM:  { int c = cap; return wzip_compress(src, n, dst, &c, level); }
	case LZ4F:   return LZ4_compress_fast((const char*)src, (char*)dst, n, cap, level);
	case LZ4HC:  return LZ4_compress_HC_extStateHC(hcState, (const char*)src, (char*)dst, n, cap, level);
	case ZSTD:   { size_t r = ZSTD_compressCCtx(zc, dst, cap, src, n, level); return ZSTD_isError(r) ? 0 : (int)r; }
	case WLZ4F:  return (int)WLZ_Compress_Fast(ws, (const char*)src, (char*)dst, n, cap, level);
	default:     return (int)WLZhc_Compress(hs, (const char*)src, (char*)dst, n, cap, level);
	}
}

static int decompress1(const unsigned char* src, int cs, unsigned char* dst, int n, int cap)
{
	switch (kind) {
	case WZIPS:  return WZIPS_decompress(src, cs, dst, n);
	case WZIPM:  { int c = n; return checked ? wzip_decompress(src, cs, dst, &c) : wzip_decompress_trusted(src, cs, dst, &c); }
	case LZ4F: case LZ4HC: return LZ4_decompress_safe((const char*)src, (char*)dst, cs, n);
	case ZSTD:   { size_t r = ZSTD_decompressDCtx(zd, dst, n, src, cs); return ZSTD_isError(r) ? -1 : (int)r; }
	default:     return (int)(checked ? WLZ_Decompress((const char*)src, (char*)dst, cs, cap)
	                                  : WLZ_Decompress_Trusted((const char*)src, (char*)dst, cs, cap));
	}
}

int main(int argc, char** argv)
{
	if (argc < 6) { fprintf(stderr, "usage: bench_blocks <codec> <level> <block size> <rounds> file...\n"); return 1; }
	codec = argv[1]; level = atoi(argv[2]);
	checked = getenv("CHECKED") != NULL;
	static const char* const names[] = { "wzips", "wzipm", "lz4", "lz4hc", "zstd", "wlz4f", "wlz4hc" };
	for (kind = 0; kind < 7 && strcmp(codec, names[kind]); kind++) ;
	if (kind == 7) { fprintf(stderr, "unknown codec %s\n", codec); return 1; }
	hcState = malloc(LZ4_sizeofStateHC());
	const int bs = atoi(argv[3]), rounds = atoi(argv[4]);
	SetThreadAffinityMask(GetCurrentThread(), 1 << 2);
	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	zc = ZSTD_createCCtx(); zd = ZSTD_createDCtx(); sc = WZIPS_createCCtx(); ws = WLZ_New_State(); hs = WLZhc_New_State();

	/* the blocks: each file cut from its start; a file's last block may be shorter */
	size_t total = 0;
	unsigned char* data = NULL;
	int nb = 0, *blen = NULL; size_t* boff = NULL;
	for (int i = 5; i < argc; i++) {
		FILE* f = fopen(argv[i], "rb");
		if (!f) { fprintf(stderr, "cannot open %s\n", argv[i]); return 1; }
		fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
		data = (unsigned char*)realloc(data, total + n);
		if (fread(data + total, 1, n, f) != (size_t)n) { fprintf(stderr, "cannot read %s\n", argv[i]); return 1; }
		fclose(f);
		for (long p = 0; p < n; p += bs) {
			blen = (int*)realloc(blen, (nb + 1) * sizeof(int)); boff = (size_t*)realloc(boff, (nb + 1) * sizeof(size_t));
			blen[nb] = (int)(n - p < bs ? n - p : bs); boff[nb] = total + p; nb++;
		}
		total += n;
	}
	const int slot = bs + 1024;                          /* room for any codec's worst case on one block */
	unsigned char* cmp = (unsigned char*)malloc((size_t)nb * slot);
	unsigned char* out = (unsigned char*)malloc(bs + 64);
	int* clen = (int*)malloc(nb * sizeof(int));

	for (double t0 = now(); now() - t0 < 1.0; )          /* warm-up */
		for (int b = 0; b < nb && b < 256; b++) compress1(data + boff[b], blen[b], cmp, slot);

	double bestC = 1e30, bestD = 1e30;
	size_t csum = 0;
	for (int r = 0; r < rounds; r++) {
		const double t = now();
		csum = 0;
		for (int b = 0; b < nb; b++) {
			clen[b] = compress1(data + boff[b], blen[b], cmp + (size_t)b * slot, slot);
			csum += clen[b];
		}
		const double e = now() - t;
		if (e < bestC) bestC = e;
	}
	int fails = 0;
	for (int b = 0; b < nb; b++) {
		if (clen[b] <= 0) { fails++; continue; }
		const int d = decompress1(cmp + (size_t)b * slot, clen[b], out, blen[b], bs + 64);
		if (d != blen[b] || memcmp(out, data + boff[b], blen[b])) fails++;
	}
	for (int r = 0; r < 10; r++) {
		const double t = now();
		for (int b = 0; b < nb; b++) decompress1(cmp + (size_t)b * slot, clen[b], out, blen[b], bs + 64);
		const double e = now() - t;
		if (e < bestD) bestD = e;
	}
	printf("%-7s %3d  block %5d  ratio %.4f  comp %8.2f MB/s  dec %7.1f MB/s  blocks %d  fails %d\n", codec, level, bs,
	       (double)total / csum, total / 1e6 / bestC, total / 1e6 / bestD, nb, fails);
	return fails != 0;
}
