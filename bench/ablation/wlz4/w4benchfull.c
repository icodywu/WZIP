/*
 * Ablation harness for the WLZ4 variants (Windows): whole files up to W4MAXN bytes, one block each.
 * Copyright (c) 2019-present, Yingquan (Cody) Wu.
 * SPDX-License-Identifier: BSD-2-Clause (see LICENSE)
 *
 * Ratio, compression and decompression speed (best of rounds; W4PIN=core pins the thread for timing), every file
 * verified. Each file is one block of its first W4MAXN bytes (default 8 MB; run.sh sets 52000000).
 * Usage: w4benchfull <codec> <level> <rounds> file...
 * codec: lz4 (LZ4_compress_fast, level = acceleration), lz4hc (level 1-12),
 * wlz (WLZ_Compress, lazy), wlzf (WLZ_Compress_Fast, level = acceleration), wlzhc (level 0-9 or more)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "lz4.h"
#include "lz4hc.h"
#include "WLZ4.h"

static double now(void) { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart / f.QuadPart; }

int main(int argc, char** argv)
{
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    { const char* pin = getenv("W4PIN"); if (pin) SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << atoi(pin)); }   /* W4PIN=core: pin for timing */
    const char* codec = argv[1];
    const int level = atoi(argv[2]), rounds = atoi(argv[3]);
    const char* mx = getenv("W4MAXN"); const int maxN = mx ? atoi(mx) : 8 << 20, cap = maxN + maxN / 64 + 4096;
    char* src = malloc(maxN + 64), *cmp = malloc(cap), *dec = malloc(maxN + 4096);
    WLZ_State_Str* ws = WLZ_New_State();
    WLZhc_State_Str* hs = WLZhc_New_State();
    double totIn = 0, totOut = 0, tc = 0, td = 0;
    int fails = 0;
    for (int f = 4; f < argc; f++) {
        FILE* fp = fopen(argv[f], "rb");
        if (!fp) { printf("cannot open %s\n", argv[f]); return 1; }
        const int n = (int)fread(src, 1, maxN, fp); fclose(fp);
        int cs = 0;
        double best = 1e9;
        for (int r = 0; r < rounds; r++) {
            const double t0 = now();
            if (!strcmp(codec, "lz4")) cs = LZ4_compress_fast(src, cmp, n, cap, level);
            else if (!strcmp(codec, "lz4hc")) cs = LZ4_compress_HC(src, cmp, n, cap, level);
            else if (!strcmp(codec, "wlz")) cs = (int)WLZ_Compress(ws, src, cmp, n, cap);
            else if (!strcmp(codec, "wlzf")) cs = (int)WLZ_Compress_Fast(ws, src, cmp, n, cap, level);
            else if (!strcmp(codec, "wlzhc")) cs = (int)WLZhc_Compress(hs, src, cmp, n, cap, level);
            const double t = now() - t0;
            if (t < best) best = t;
        }
        tc += best;
        double bestd = 1e9;
        int d = 0;
        for (int r = 0; r < rounds * 3; r++) {
            memset(dec, 0, 64);
            const double t0 = now();
            if (!strncmp(codec, "lz4", 3)) d = LZ4_decompress_safe(cmp, dec, cs, maxN + 4096);
            else d = (int)WLZ_Decompress(cmp, dec, cs, maxN + 4096);
            const double t = now() - t0;
            if (t < bestd) bestd = t;
        }
        td += bestd;
        if (d != n || memcmp(dec, src, n)) fails++;
        totIn += n; totOut += cs;
    }
    printf("%-6s %2d  ratio %.4f  comp %8.2f MB/s  dec %8.1f MB/s  fails %d\n", codec, level, totIn / totOut, totIn / tc / 1e6, totIn / td / 1e6, fails);
    return 0;
}
