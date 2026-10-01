/*
 * Match mix of the far-code optimal parse (WLZ_VARIANT 0): matches by length and offset class, and what the same
 * parse would cost with offset bytes by length alone (variants 1 and 2, and 3-byte offsets from length T).
 * Usage: v3stat <level> file...   (link with WLZ4_variants.c built with -DWLZ_VARIANT=0)
 * Copyright (c) 2026, Yingquan (Cody) Wu.
 * SPDX-License-Identifier: BSD-2-Clause (see LICENSE)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "WLZ4.h"

static const unsigned char CodeLen[16] = { 3, 4, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 6 };
static const unsigned char CodeOff[16] = { 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3 };
static const unsigned char CodeExt[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1 };

static unsigned readExt(const unsigned char** pp)
{
    const unsigned char* p = *pp;
    unsigned v = *p++;
    if (v > 251) { const int n = v - 251; unsigned x = 0; for (int i = 0; i < n; i++) x |= (unsigned)p[i] << (8 * i); p += n; v = 252 + x; }
    *pp = p;
    return v;
}
static int extSize(unsigned v) { return v < 252 ? 1 : 2 + (v - 252 > 0xFF) + (v - 252 > 0xFFFF) + (v - 252 > 0xFFFFFF); }

static double cnt[5][3], saved[5][3], matchBytes, litBytes, total, out, d1, d2, dT[5];
static const int Ts[5] = { 6, 8, 10, 12, 16 };
static int lclass(unsigned l) { return l == 3 ? 0 : l == 4 ? 1 : l == 5 ? 2 : l < 16 ? 3 : 4; }
static int oclass(unsigned o) { return o < 256 ? 0 : o < 65536 ? 1 : 2; }

/* length rule with threshold T: len 3 -> 1B, len 4..T-1 -> 2B, len >= T -> 3B; 16 codes: 3..17 plain, 18+ext */
static int costT(int T, unsigned len, unsigned off)
{
    const int ob = len == 3 ? 1 : (int)len < T ? 2 : 3;
    const unsigned win = ob == 1 ? 256u : ob == 2 ? 65536u : 16777216u;
    if (off >= win) return -1;
    return ob + (len >= 18 ? extSize(len - 18) : 0);
}

/* match-part bytes (offset + length extension) in variant v; -1: not codable */
static int cost(int v, unsigned len, unsigned off)
{
    if (v == 1) {
        if (len == 3) return off < 256 ? 1 : -1;
        if (len <= 5) return off < 65536 ? 2 : -1;
        return 3 + (len >= 18 ? extSize(len - 18) : 0);
    }
    if (len == 3) return -1;
    if (len <= 5) return off < 65536 ? 2 : -1;
    return 3 + (len >= 19 ? extSize(len - 19) : 0);
}

int main(int argc, char** argv)
{
    const int level = atoi(argv[1]);
    const int maxN = 52000000;
    unsigned char* src = malloc(maxN + 64), *cmp = malloc(maxN + maxN / 64 + 4096);
    WLZhc_State_Str* hs = WLZhc_New_State();
    for (int f = 2; f < argc; f++) {
        FILE* fp = fopen(argv[f], "rb");
        const int n = (int)fread(src, 1, maxN, fp); fclose(fp);
        const int cs = (int)WLZhc_Compress(hs, (const char*)src, (char*)cmp, n, maxN + maxN / 64 + 4096, level);
        total += n; out += cs;
        const unsigned char* p = cmp + ((cmp[1] & 0x80) ? 4 : 2);
        while (1) {
            const unsigned token = *p++;
            unsigned ll = token >> 4;
            if (ll == 15) ll += readExt(&p);
            p += ll; litBytes += ll;
            const unsigned code = token & 15;
            unsigned off = 0;
            for (int i = 0; i < CodeOff[code]; i++) off |= (unsigned)p[i] << (8 * i);
            p += CodeOff[code];
            if (!off) break;
            unsigned len = CodeLen[code];
            int c0 = CodeOff[code];
            if (CodeExt[code]) { const unsigned e = readExt(&p); len += e; c0 += extSize(e); }
            const int li = lclass(len), oi = oclass(off);
            cnt[li][oi]++; saved[li][oi] += (double)len - 1 - c0; matchBytes += len;
            const int c1 = cost(1, len, off), c2 = cost(2, len, off);
            d1 += c1 < 0 ? (int)len - c0 - 1 : c1 - c0;   /* not codable: its bytes as literals (its token merges away) */
            d2 += c2 < 0 ? (int)len - c0 - 1 : c2 - c0;
            for (int t = 0; t < 5; t++) { const int ct = costT(Ts[t], len, off); dT[t] += ct < 0 ? (int)len - c0 - 1 : ct - c0; }
        }
    }
    static const char* ln[5] = { "len 3   ", "len 4   ", "len 5   ", "len 6-15", "len 16+ " };
    printf("input %.0f  v2 output %.0f (ratio %.4f)  literals %.2f%%\n", total, out, total / out, 100 * litBytes / total);
    printf("            %26s %26s %26s\n", "offset < 256", "256 .. 64K", "64K .. 16M");
    for (int l = 0; l < 5; l++) {
        printf("  %s", ln[l]);
        for (int o = 0; o < 3; o++) printf("   n %9.0f (%5.2f%%)    ", cnt[l][o], 100 * cnt[l][o] / (cnt[0][0] + cnt[0][1] + cnt[1][0] + cnt[1][1] + cnt[2][1] + cnt[3][0] + cnt[3][1] + cnt[3][2] + cnt[4][0] + cnt[4][1] + cnt[4][2] + cnt[2][0] + cnt[2][2] + cnt[1][2] + cnt[0][2]));
        printf("\n");
    }
    printf("same parse repriced: variant 1 %+.0f bytes (ratio %.4f), variant 2 %+.0f bytes (ratio %.4f)\n",
           d1, total / (out + d1), d2, total / (out + d2));
    for (int t = 0; t < 5; t++) printf("  length rule, 3 offset bytes from length %2d: ratio %.4f\n", Ts[t], total / (out + dT[t]));
    return 0;
}
