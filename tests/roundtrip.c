/*
 * Round-trip test for WZIP (L/M through wzip_compress), WZIP_S and WLZ4.
 * Copyright (c) 2018-present, Yingquan (Cody) Wu. SPDX-License-Identifier: BSD-2-Clause
 *
 * usage: roundtrip [file ...]
 *   Without files, runs a built-in set of synthetic inputs (empty, tiny, runs, periodic, random, text-like).
 *   Every codec and level is checked: the output must match the input exactly, and the decoder must not write
 *   past the capacity it is given (a guard area after the buffer is verified). Exit status 0 means all passed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "WZIP.h"
#include "WLZ4.h"
#include "wzframe.h"

#define GUARD 64
static int failures, checks;

static void fail(const char* what, const char* name, int level, const char* why)
{
	printf("FAIL %-8s level %2d  %s: %s\n", what, level, name, why);
	failures++;
}

/* a buffer of n bytes followed by a guard area filled with a known byte */
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

static void test_wzip(const unsigned char* src, int n, const char* name)
{
	const int bound = WZIP_Cap_CmprSize(n);
	unsigned char* cmp = guarded(bound);
	unsigned char* dec = guarded(n > 0 ? n : 1);
	for (int level = 0; level <= 13; level++) {
		int cap = bound;
		const int c = wzip_compress(src, n, cmp, &cap, level);
		checks++;
		if (c <= 0 || c > n + 2) { fail("wzip", name, level, "compression failed or expanded by more than 2 bytes"); continue; }
		if (!guard_ok(cmp, bound)) fail("wzip", name, level, "encoder wrote past its capacity");
		int dcap = n;                                     /* exactly the decoded size */
		const int d = wzip_decompress(cmp, c, dec, &dcap);
		if (d != n || memcmp(src, dec, n)) fail("wzip", name, level, "decoded data differs");
		if (!guard_ok(dec, n > 0 ? n : 1)) fail("wzip", name, level, "decoder wrote past the decoded size");
		{   /* trusted mode, from a buffer holding the stream and WZIP_TRUSTED_SRC_PAD bytes */
			unsigned char* t = (unsigned char*)malloc((size_t)c + WZIP_TRUSTED_SRC_PAD);
			memcpy(t, cmp, c);
			memset(t + c, 0, WZIP_TRUSTED_SRC_PAD);
			memset(dec, 0, n > 0 ? n : 1);
			dcap = n;
			if (wzip_decompress_trusted(t, c, dec, &dcap) != n || memcmp(src, dec, n)) fail("wzip", name, level, "trusted mode differs");
			if (!guard_ok(dec, n > 0 ? n : 1)) fail("wzip", name, level, "trusted mode wrote past the decoded size");
			free(t);
		}
		if (n > 0) {                                      /* one byte too little room must fail cleanly */
			dcap = n - 1;
			if (wzip_decompress(cmp, c, dec, &dcap) != 0) fail("wzip", name, level, "accepted a too-small output buffer");
		}
	}
	/* a small output buffer: either a valid stream that fits, or 0 */
	if (n >= 64) {
		int cap = n / 2;
		const int c = wzip_compress(src, n, cmp, &cap, 9);
		checks++;
		if (c > n / 2) fail("wzip", name, 9, "exceeded a small output capacity");
		if (c > 0) {
			int dcap = n;
			if (wzip_decompress(cmp, c, dec, &dcap) != n || memcmp(src, dec, n)) fail("wzip", name, 9, "small-capacity stream differs");
		}
	}
	free(cmp); free(dec);
}

static void test_wzips(const unsigned char* src, int n, const char* name)
{
	WZIPS_CCtx* cctx = WZIPS_createCCtx();
	const int blocks[2] = { 4096, 8192 };
	for (int b = 0; b < 2; b++) {
		const int bs = blocks[b];
		unsigned char* cmp = guarded(WZIPS_COMPRESSBOUND(bs));
		unsigned char* dec = guarded(bs);
		for (int level = 1; level <= 9; level++) {
			for (int pos = 0; pos < n; pos += bs) {
				const int len = n - pos < bs ? n - pos : bs;
				/* the preceding bytes, up to WZIPS_MAX_DICT, serve as the dictionary on odd levels */
				const int dictSize = (level & 1) ? (pos < WZIPS_MAX_DICT ? pos : WZIPS_MAX_DICT) : 0;
				WZIPS_CDict* cdict = dictSize ? WZIPS_createCDict(src + pos - dictSize, dictSize) : NULL;
				const int c = WZIPS_compress_usingCDict(cctx, cdict, src + pos, len, cmp, WZIPS_COMPRESSBOUND(len), level);
				checks++;
				if (c <= 0 || c > WZIPS_COMPRESSBOUND(len)) { fail("wzip_s", name, level, "compression failed"); WZIPS_freeCDict(cdict); continue; }
				memset(dec + len, 0xA5, GUARD);
				const int d = WZIPS_decompress_usingDict(cmp, c, dec, len, src + pos - dictSize, dictSize);
				if (d != len || memcmp(src + pos, dec, len)) fail("wzip_s", name, level, "decoded data differs");
				if (!guard_ok(dec, len)) fail("wzip_s", name, level, "decoder wrote past the block");
				WZIPS_freeCDict(cdict);
			}
		}
		free(cmp); free(dec);
	}
	WZIPS_freeCCtx(cctx);
}

static void test_wlz4(const unsigned char* src, int n, const char* name)
{
	const unsigned bound = WLZ_COMPRESSBOUND((unsigned)n);
	unsigned char* cmp = guarded(bound);
	unsigned char* dec = guarded((size_t)n + WLZ_MEM_OVERHEAD);
	WLZ_State_Str* ws = WLZ_New_State();
	WLZhc_State_Str* hs = WLZhc_New_State();
	for (int mode = -2; mode <= 12; mode++) {            /* -2 fast, -1 lazy, 0..12 hash-chain and optimal levels */
		unsigned c;
		if (mode == -2)      c = WLZ_Compress_Fast(ws, (const char*)src, (char*)cmp, (unsigned)n, bound, 1);
		else if (mode == -1) c = WLZ_Compress(ws, (const char*)src, (char*)cmp, (unsigned)n, bound);
		else                 c = WLZhc_Compress(hs, (const char*)src, (char*)cmp, (unsigned)n, bound, mode);
		checks++;
		if (c == 0 || c > bound) { fail("wlz4", name, mode, "compression failed"); continue; }
		if (!guard_ok(cmp, bound)) fail("wlz4", name, mode, "encoder wrote past its capacity");
		if (WLZ_Read_DecSize((const char*)cmp, c) != (unsigned)n) fail("wlz4", name, mode, "wrong size header");
		const unsigned d = WLZ_Decompress((const char*)cmp, (char*)dec, c, (unsigned)n + WLZ_MEM_OVERHEAD);
		if (d != (unsigned)n || memcmp(src, dec, n)) fail("wlz4", name, mode, "decoded data differs");
		if (!guard_ok(dec, (size_t)n + WLZ_MEM_OVERHEAD)) fail("wlz4", name, mode, "decoder wrote past its capacity");
		{   /* trusted mode, from a buffer holding the stream and WLZ_TRUSTED_SRC_PAD bytes */
			unsigned char* t = (unsigned char*)malloc((size_t)c + WLZ_TRUSTED_SRC_PAD);
			memcpy(t, cmp, c);
			memset(t + c, 0, WLZ_TRUSTED_SRC_PAD);
			memset(dec, 0, (size_t)n + WLZ_MEM_OVERHEAD);
			const unsigned dt = WLZ_Decompress_Trusted((const char*)t, (char*)dec, c, (unsigned)n + WLZ_MEM_OVERHEAD);
			if (dt != (unsigned)n || memcmp(src, dec, n)) fail("wlz4", name, mode, "trusted mode differs");
			if (!guard_ok(dec, (size_t)n + WLZ_MEM_OVERHEAD)) fail("wlz4", name, mode, "trusted mode wrote past its capacity");
			free(t);
		}
	}
	WLZ_Free_State(ws);
	WLZhc_Free_State(hs);
	free(cmp); free(dec);
}

static unsigned rd32le(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

/* decodes a frame block by block through the streaming functions; returns the content size or an error code */
static size_t stream_decode(const unsigned char* f, size_t fs, unsigned char* out, size_t cap)
{
	WZF_DCtx* d = WZF_createDCtx();
	size_t pos = 0, o = 0, r = WZF_decompressBegin(d, f, fs < WZF_HEADER_MIN ? fs : WZF_HEADER_MIN);
	if (!WZF_isError(r) && r > WZF_HEADER_MIN) r = WZF_decompressBegin(d, f, r);
	if (WZF_isError(r)) { WZF_freeDCtx(d); return r; }
	pos = r;
	for (;;) {
		int raw;
		const size_t c = WZF_nextBlock(d, f + pos, &raw);
		if (WZF_isError(c)) { WZF_freeDCtx(d); return c; }
		pos += WZF_BLOCK_HEADER;
		if (c == 0) break;
		r = WZF_decompressBlock(d, out + o, cap - o, f + pos, c);
		if (WZF_isError(r)) { WZF_freeDCtx(d); return r; }
		o += r; pos += c;
	}
	r = WZF_decompressEnd(d, f + pos, WZF_endSize(d));
	WZF_freeDCtx(d);
	return WZF_isError(r) ? r : o;
}

static void test_frames(const unsigned char* src, int n, const char* name)
{
	static const int wzipLevels[] = { 0, 1, 5, 11 }, wlz4Levels[] = { -2, -1, 2, 10 };
	static const int blockLogs[] = { 0, 10, 16 };
	for (int codec = 0; codec <= 1; codec++)
	for (int li = 0; li < 4; li++)
	for (int bi = 0; bi < 3; bi++) {
		const int level = codec ? wlz4Levels[li] : wzipLevels[li];
		if (bi && n > (1 << 18) && level > 5) continue;  /* small blocks at slow levels: enough on the smaller inputs */
		WZF_params p = { codec, level, blockLogs[bi], (li & 1) };     /* checksum on some, off on others */
		const size_t bound = WZF_compressBound((size_t)n, &p);
		unsigned char* f = guarded(bound);
		unsigned char* dec = guarded(n > 0 ? n : 1);
		const char* what = codec ? "frame/wlz4" : "frame/wzip";
		const size_t fs = WZF_compress(f, bound, src, (size_t)n, &p);
		checks++;
		if (WZF_isError(fs)) { fail(what, name, level, WZF_getErrorName(fs)); free(f); free(dec); continue; }
		if (!guard_ok(f, bound)) fail(what, name, level, "encoder wrote past its capacity");
		if (WZF_getContentSize(f, fs) != (unsigned long long)n) fail(what, name, level, "wrong content size");
		size_t d = WZF_decompress(dec, (size_t)n, f, fs);
		if (d != (size_t)n || memcmp(src, dec, n)) fail(what, name, level, WZF_isError(d) ? WZF_getErrorName(d) : "decoded data differs");
		if (!guard_ok(dec, n > 0 ? n : 1)) fail(what, name, level, "decoder wrote past the content size");
		memset(dec, 0, n > 0 ? n : 1);
		d = stream_decode(f, fs, dec, (size_t)n);
		if (d != (size_t)n || memcmp(src, dec, n)) fail(what, name, level, "block-by-block decoding differs");
		if (!(li & 1) && rd32le(f + fs - 4) != WZF_XXH32(src, (size_t)n, 0)) fail(what, name, level, "checksum differs from XXH32");
		if (n > 0) {
			if (WZF_getErrorCode(WZF_decompress(dec, (size_t)n - 1, f, fs)) != WZF_error_dstSize_tooSmall)
				fail(what, name, level, "accepted a too-small output buffer");
			if (WZF_getErrorCode(WZF_decompress(dec, (size_t)n, f, fs - 1)) != WZF_error_srcSize_wrong)
				fail(what, name, level, "accepted a truncated frame");
			if (!(li & 1)) {                              /* a damaged checksum is caught */
				f[fs - 1] ^= 1;
				if (WZF_getErrorCode(WZF_decompress(dec, (size_t)n, f, fs)) != WZF_error_checksum)
					fail(what, name, level, "accepted a wrong checksum");
				f[fs - 1] ^= 1;
			}
		}
		free(f); free(dec);
	}

	/* irregular blocks through the streaming compressor, two frames and a skippable frame in one stream */
	if (n > 0) {
		WZF_params p = { WZF_CODEC_WLZ4, -1, 12, 0 };
		WZF_CCtx* c = WZF_createCCtx();
		const size_t cap = 2 * WZF_compressBound((size_t)n, &p) + (size_t)n / 64 * WZF_BLOCK_HEADER + 64;
		unsigned char* f = guarded(cap);
		unsigned char* dec = guarded(2 * (size_t)n);
		size_t pos = 0;
		for (int frame = 0; frame < 2; frame++) {
			p.codec = frame;                              /* a WZIP frame (level 1), then a WLZ4 frame (lazy) */
			p.level = frame ? -1 : 1;
			const size_t h = WZF_compressBegin(c, f + pos, cap - pos, &p, frame ? WZF_CONTENTSIZE_UNKNOWN : (unsigned long long)n);
			if (WZF_isError(h)) { fail("frame/stream", name, frame, WZF_getErrorName(h)); break; }
			pos += h;
			for (int done = 0, k = 0; done < n; k++) {
				const int len = n - done < 1 + (k * 977) % 4096 ? n - done : 1 + (k * 977) % 4096;
				const size_t r = WZF_compressBlock(c, f + pos, cap - pos, src + done, len);
				if (WZF_isError(r)) { fail("frame/stream", name, k, WZF_getErrorName(r)); break; }
				pos += r; done += len;
			}
			pos += WZF_compressEnd(c, f + pos, cap - pos);
			if (rd32le(f + pos - 4) != WZF_XXH32(src, (size_t)n, 0)) fail("frame/stream", name, frame, "checksum differs from XXH32");
			if (!frame) {                                 /* a skippable frame between the two */
				memcpy(f + pos, "\x5A\x2A\x4D\x18\x03\x00\x00\x00xyz", 11);
				pos += 11;
			}
		}
		checks++;
		const size_t d = WZF_decompress(dec, 2 * (size_t)n, f, pos);
		if (d != 2 * (size_t)n || memcmp(src, dec, n) || memcmp(src, dec + n, n))
			fail("frame/stream", name, 0, WZF_isError(d) ? WZF_getErrorName(d) : "decoded data differs");
		if (WZF_getContentSize(f, pos) != WZF_CONTENTSIZE_UNKNOWN) fail("frame/stream", name, 0, "content size should be unknown");
		WZF_freeCCtx(c);
		free(f); free(dec);
	}
}

static void test_all(const unsigned char* src, int n, const char* name)
{
	const int before = failures;
	test_wzip(src, n, name);
	if (n > 0) test_wzips(src, n, name);
	test_wlz4(src, n, name);
	test_frames(src, n, name);
	printf("%-28s %10d bytes  %s\n", name, n, failures == before ? "ok" : "FAILED");
	fflush(stdout);
}

static unsigned rng = 12345;
static unsigned next_rand(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static void synthetic(void)
{
	static const char words[][8] = { "the ", "match ", "window ", "length ", "offset ", "of ", "a ", "and ", "LZ77 ", "code " };
	const int n = 1 << 20;
	unsigned char* b = (unsigned char*)calloc(n, 1);
	char name[64];

	test_all(b, 0, "empty");
	b[0] = 'x'; test_all(b, 1, "one byte");
	for (int i = 0; i < 31; i++) b[i] = (unsigned char)("abcabcabd"[i % 9]);
	test_all(b, 31, "31 bytes");
	test_all(b, 32, "32 bytes");
	memset(b, 0, n); test_all(b, 70000, "zeros 70000");
	for (int i = 0; i < n; i++) b[i] = (unsigned char)next_rand();
	test_all(b, 40000, "random 40000");
	test_all(b, 300000, "random 300000");
	for (int i = 0; i < n; i++) b[i] = (unsigned char)(i % 251 < 7 ? 'a' + i % 7 : i % 17);
	test_all(b, n, "periodic 1 MB");
	for (int p = 0; p < n; ) {                           /* text-like: random words */
		const char* w = words[next_rand() % 10];
		for (int k = 0; w[k] && p < n; k++) b[p++] = (unsigned char)w[k];
	}
	for (int size = 1000; size <= n; size *= 4) {
		snprintf(name, sizeof name, "words %d", size);
		test_all(b, size, name);
	}
	for (int i = 0; i < n; i++)                          /* long runs with literal bursts */
		b[i] = (unsigned char)((i / 1000) & 1 ? next_rand() : (i / 2000) & 0xFF);
	test_all(b, n, "runs and bursts 1 MB");
	free(b);
}

int main(int argc, char** argv)
{
	if (argc < 2) synthetic();
	for (int i = 1; i < argc; i++) {
		FILE* f = fopen(argv[i], "rb");
		if (!f) { printf("cannot open %s\n", argv[i]); failures++; continue; }
		fseek(f, 0, SEEK_END);
		const long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		unsigned char* b = (unsigned char*)malloc(n > 0 ? n : 1);
		if (!b || fread(b, 1, n, f) != (size_t)n) { printf("cannot read %s\n", argv[i]); failures++; fclose(f); free(b); continue; }
		fclose(f);
		test_all(b, (int)n, argv[i]);
		free(b);
	}
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
