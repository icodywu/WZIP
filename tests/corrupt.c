/*
 * Corrupt-input test for the WZIP, WZIP_S and WLZ4 decoders and the WZ frame decoder.
 * Copyright (c) 2018-present, Yingquan (Cody) Wu. SPDX-License-Identifier: BSD-2-Clause
 *
 * usage: corrupt [iterations [seed]]
 *   Compresses synthetic inputs with every codec and a spread of levels, then decodes damaged copies of each stream:
 *   flipped bits, overwritten bytes, truncations, spliced ranges and appended garbage. Each damaged stream sits in a
 *   buffer of exactly its size, so a sanitizer reports any read past it; the output buffer is followed by a guard area.
 *   A decoder may reject the stream or return some output, but must stay inside its buffers. Build with
 *   -fsanitize=address,undefined for the full check (make fuzz does). Exit status 0 means no guard was touched.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "WZIP.h"
#include "WLZ4.h"
#include "wzframe.h"

#define GUARD 64
static int failures, decodes;
static unsigned rng;
static unsigned next_rand(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static unsigned char* guarded(size_t n)
{
	unsigned char* p = (unsigned char*)malloc(n + GUARD);
	if (!p) { printf("out of memory\n"); exit(2); }
	memset(p + n, 0xA5, GUARD);
	return p;
}

static int guard_ok(const unsigned char* p, size_t n)
{
	for (int i = 0; i < GUARD; i++)
		if (p[n + i] != 0xA5) return 0;
	return 1;
}

/* a damaged copy of cmp[0..c) in a buffer of exactly its size; returns its size */
static int damage(const unsigned char* cmp, int c, unsigned char** out)
{
	int len = c;
	unsigned char* d;
	const int kind = (int)(next_rand() % 6);
	if (kind == 4) len = (int)(next_rand() % (unsigned)c);                   /* truncate */
	if (kind == 5) len = c + 1 + (int)(next_rand() % 64);                    /* append garbage */
	d = (unsigned char*)malloc(len > 0 ? len : 1);
	memcpy(d, cmp, len < c ? len : c);
	for (int i = c; i < len; i++) d[i] = (unsigned char)next_rand();
	if (kind == 0 || kind == 5) {                                            /* flip a few bits */
		const int k = 1 + (int)(next_rand() % 4);
		for (int i = 0; i < k && len > 0; i++) d[next_rand() % len] ^= (unsigned char)(1u << (next_rand() % 8));
	}
	else if (kind == 1 && len > 0) {                                         /* overwrite a few bytes */
		const int k = 1 + (int)(next_rand() % 8);
		for (int i = 0; i < k; i++) d[next_rand() % len] = (unsigned char)next_rand();
	}
	else if (kind == 2 && len > 0) {                                         /* damage the first bytes (headers) */
		const int k = 1 + (int)(next_rand() % 3);
		for (int i = 0; i < k; i++) d[next_rand() % (len < 24 ? len : 24)] = (unsigned char)next_rand();
	}
	else if (kind == 3 && len > 8) {                                         /* copy a range over another */
		const int span = 1 + (int)(next_rand() % (len / 2));
		const int from = (int)(next_rand() % (len - span + 1)), to = (int)(next_rand() % (len - span + 1));
		memmove(d + to, d + from, span);
	}
	*out = d;
	return len;
}

static void report(const char* codec, int level, const char* name, int it)
{
	printf("FAIL %-6s level %2d  %s  iteration %d: decoder wrote past its buffer\n", codec, level, name, it);
	failures++;
}

/* the undamaged stream, in a buffer of exactly its size plus pad bytes: the decoder must reproduce the input without
   reading further (pad 0 for the checked decoders, the documented slack for the trusted ones) */
static unsigned char* exact_copy_pad(const unsigned char* cmp, int c, int pad)
{
	unsigned char* d = (unsigned char*)malloc((size_t)c + pad + 1);
	memcpy(d, cmp, c);
	memset(d + c, 0, pad);
	return d;
}
static unsigned char* exact_copy(const unsigned char* cmp, int c) { return exact_copy_pad(cmp, c, 0); }
static void check_clean(int ok, const char* codec, int level, const char* name)
{
	decodes++;
	if (!ok) { printf("FAIL %-6s level %2d  %s: the undamaged stream did not decode\n", codec, level, name); failures++; }
}

static void fuzz_wzip(const unsigned char* src, int n, const char* name, int iters)
{
	static const int levels[] = { 0, 1, 3, 6, 9, 11, 12, 13 };
	const int bound = WZIP_Cap_CmprSize(n);
	unsigned char* cmp = (unsigned char*)malloc(bound);
	unsigned char* dec = guarded(n);
	for (unsigned l = 0; l < sizeof levels / sizeof levels[0]; l++) {
		int cap = bound;
		const int c = wzip_compress(src, n, cmp, &cap, levels[l]);
		if (c <= 0) { printf("FAIL wzip level %d %s: compression failed\n", levels[l], name); failures++; continue; }
		{
			unsigned char* e = exact_copy(cmp, c);
			int dcap = n;
			check_clean(wzip_decompress(e, c, dec, &dcap) == n && !memcmp(dec, src, n), "wzip", levels[l], name);
			free(e);
			e = exact_copy_pad(cmp, c, WZIP_TRUSTED_SRC_PAD);      /* trusted mode: undamaged streams only */
			dcap = n;
			check_clean(wzip_decompress_trusted(e, c, dec, &dcap) == n && !memcmp(dec, src, n) && guard_ok(dec, n), "wzip-t", levels[l], name);
			free(e);
		}
		for (int it = 0; it < iters; it++) {
			unsigned char* d;
			const int len = damage(cmp, c, &d);
			int dcap = n;
			memset(dec + n, 0xA5, GUARD);
			wzip_decompress(d, len, dec, &dcap);
			decodes++;
			if (!guard_ok(dec, n)) report("wzip", levels[l], name, it);
			free(d);
		}
	}
	free(cmp); free(dec);
}

static void fuzz_wzips(const unsigned char* src, int n, const char* name, int iters)
{
	WZIPS_CCtx* cctx = WZIPS_createCCtx();
	const int bs = n < 8192 ? n : 8192;
	unsigned char* cmp = (unsigned char*)malloc(WZIPS_COMPRESSBOUND(bs));
	unsigned char* dec = guarded(bs);
	for (int level = 1; level <= 9; level += 4) {
		const int useDict = level != 5 && n >= 2 * bs;
		const unsigned char* const blk = src + (useDict ? bs : 0);
		WZIPS_CDict* cdict = useDict ? WZIPS_createCDict(src, bs) : NULL;
		const int c = WZIPS_compress_usingCDict(cctx, cdict, blk, bs, cmp, WZIPS_COMPRESSBOUND(bs), level);
		WZIPS_freeCDict(cdict);
		if (c <= 0) { printf("FAIL wzip_s level %d %s: compression failed\n", level, name); failures++; continue; }
		{
			unsigned char* e = exact_copy(cmp, c);
			check_clean(WZIPS_decompress_usingDict(e, c, dec, bs, useDict ? src : NULL, useDict ? bs : 0) == bs && !memcmp(dec, blk, bs),
				"wzip_s", level, name);
			free(e);
		}
		for (int it = 0; it < iters; it++) {
			unsigned char* d;
			const int len = damage(cmp, c, &d);
			memset(dec + bs, 0xA5, GUARD);
			WZIPS_decompress_usingDict(d, len, dec, bs, useDict ? src : NULL, useDict ? bs : 0);
			decodes++;
			if (!guard_ok(dec, bs)) report("wzip_s", level, name, it);
			free(d);
		}
	}
	free(cmp); free(dec);
	WZIPS_freeCCtx(cctx);
}

static void fuzz_wlz4(const unsigned char* src, int n, const char* name, int iters)
{
	static const int modes[] = { -2, -1, 2, 6, 10, 12 };                 /* -2 fast, -1 lazy, then hash-chain levels */
	const unsigned bound = WLZ_COMPRESSBOUND((unsigned)n);
	unsigned char* cmp = (unsigned char*)malloc(bound);
	const size_t cap = (size_t)n + WLZ_MEM_OVERHEAD;
	unsigned char* dec = guarded(cap);
	WLZ_State_Str* ws = WLZ_New_State();
	WLZhc_State_Str* hs = WLZhc_New_State();
	for (unsigned m = 0; m < sizeof modes / sizeof modes[0]; m++) {
		const int mode = modes[m];
		unsigned c;
		if (mode == -2)      c = WLZ_Compress_Fast(ws, (const char*)src, (char*)cmp, (unsigned)n, bound, 1);
		else if (mode == -1) c = WLZ_Compress(ws, (const char*)src, (char*)cmp, (unsigned)n, bound);
		else                 c = WLZhc_Compress(hs, (const char*)src, (char*)cmp, (unsigned)n, bound, mode);
		if (c == 0) { printf("FAIL wlz4 mode %d %s: compression failed\n", mode, name); failures++; continue; }
		{
			unsigned char* e = exact_copy(cmp, (int)c);
			check_clean(WLZ_Decompress((const char*)e, (char*)dec, c, (unsigned)cap) == (unsigned)n && !memcmp(dec, src, n), "wlz4", mode, name);
			free(e);
			e = exact_copy_pad(cmp, (int)c, WLZ_TRUSTED_SRC_PAD);  /* trusted mode: undamaged streams only */
			check_clean(WLZ_Decompress_Trusted((const char*)e, (char*)dec, c, (unsigned)cap) == (unsigned)n && !memcmp(dec, src, n)
				&& guard_ok(dec, cap), "wlz4-t", mode, name);
			free(e);
		}
		for (int it = 0; it < iters; it++) {
			unsigned char* d;
			const int len = damage(cmp, (int)c, &d);
			memset(dec + cap, 0xA5, GUARD);
			if (it & 1) WLZ_Decompress((const char*)d, (char*)dec, (unsigned)len, (unsigned)cap);
			else WLZ_Decompress_wDict((const char*)d, (char*)dec, (unsigned)len, (unsigned)cap, (const char*)src, 4096);
			decodes++;
			if (!guard_ok(dec, cap)) report("wlz4", mode, name, it);
			free(d);
		}
	}
	WLZ_Free_State(ws);
	WLZhc_Free_State(hs);
	free(cmp); free(dec);
}

static void fuzz_frames(const unsigned char* src, int n, const char* name, int iters)
{
	static const int set[][3] = { { WZF_CODEC_WZIP, 1, 0 }, { WZF_CODEC_WZIP, 9, 13 }, { WZF_CODEC_WLZ4, -1, 12 }, { WZF_CODEC_WLZ4, 10, 0 } };
	unsigned char* dec = guarded(n);
	for (unsigned k = 0; k < sizeof set / sizeof set[0]; k++) {
		const WZF_params p = { set[k][0], set[k][1], set[k][2], 0 };
		const size_t bound = WZF_compressBound((size_t)n, &p);
		unsigned char* f = (unsigned char*)malloc(bound);
		const size_t fs = WZF_compress(f, bound, src, (size_t)n, &p);
		if (WZF_isError(fs)) { printf("FAIL frame %u %s: %s\n", k, name, WZF_getErrorName(fs)); failures++; free(f); continue; }
		{
			unsigned char* e = exact_copy(f, (int)fs);
			check_clean(WZF_decompress(dec, (size_t)n, e, fs) == (size_t)n && !memcmp(dec, src, n), "frame", (int)k, name);
			free(e);
		}
		for (int it = 0; it < iters; it++) {
			unsigned char* d;
			const int len = damage(f, (int)fs, &d);
			memset(dec + n, 0xA5, GUARD);
			const size_t r = WZF_decompress(dec, (size_t)n, d, (size_t)len);
			decodes++;
			if (!guard_ok(dec, n)) report("frame", (int)k, name, it);
			if (!WZF_isError(r) && (r != (size_t)n || memcmp(dec, src, n)) && p.noChecksum == 0 && len >= (int)fs) {
				/* a damaged frame that decodes must still match: the checksum covers the content */
				printf("FAIL frame %u %s iteration %d: damaged content passed the checksum\n", k, name, it);
				failures++;
			}
			free(d);
		}
		free(f);
	}
	free(dec);
}

static void fuzz_all(const unsigned char* src, int n, const char* name, int iters)
{
	const int before = failures;
	fuzz_wzip(src, n, name, iters);
	fuzz_wzips(src, n, name, iters);
	fuzz_wlz4(src, n, name, iters);
	fuzz_frames(src, n, name, iters);
	printf("%-24s %8d bytes  %s\n", name, n, failures == before ? "ok" : "FAILED");
	fflush(stdout);
}

int main(int argc, char** argv)
{
	static const char words[][8] = { "the ", "match ", "window ", "length ", "offset ", "of ", "a ", "and ", "LZ77 ", "code " };
	const int iters = argc > 1 ? atoi(argv[1]) : 200;
	rng = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 12345u;
	const int n = 300000;
	unsigned char* b = (unsigned char*)malloc(n);

	for (int p = 0; p < n; ) {                              /* text-like: random words */
		const char* w = words[next_rand() % 10];
		for (int k = 0; w[k] && p < n; k++) b[p++] = (unsigned char)w[k];
	}
	fuzz_all(b, 5000, "words 5000", iters);                 /* WZIP_M */
	fuzz_all(b, 30000, "words 30000", iters);
	fuzz_all(b, n, "words 300000", iters);                  /* WZIP_L, several sequence blocks */
	for (int i = 0; i < n; i++) b[i] = (unsigned char)next_rand();
	for (int i = 0; i < n; i++) if ((i / 3000) & 1) b[i] = (unsigned char)(i % 97 < 50 ? 'a' + i % 5 : b[i]);
	fuzz_all(b, 70000, "mixed 70000", iters);               /* literal-heavy blocks */
	for (int i = 0; i < n; i++) b[i] = (unsigned char)((i / 1000) & 1 ? next_rand() : (i / 2000) & 0xFF);
	fuzz_all(b, 100000, "runs 100000", iters);

	free(b);
	printf("%d damaged streams decoded, %d failures\n", decodes, failures);
	return failures ? 1 : 0;
}
