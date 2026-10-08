/*
 * Round-trip test for WZIP (L/M through wzip_compress, and WZIP_L with a dictionary), WZIP_S and WLZ4.
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
	/* threads (wzip_compress_mt, with WZIP_MULTITHREAD): the stream of one thread */
	if (n >= 32768) {
		static const int mtLevels[3] = { 7, 11, 13 };
		unsigned char* one = guarded(bound);
		for (int k = 0; k < 3; k++) {
			int cap1 = bound;
			const int c1 = wzip_compress(src, n, one, &cap1, mtLevels[k]);
			for (int w = 2; w <= 7; w += w < 4 ? 2 : 1) {      /* 2, 4, 5, 6, 7: the tree in 1 to 4 parts */
				if (w > 4 && w != 4 + k + 1 && n > (1 << 18)) continue;
				int capm = bound;
				const int cm = wzip_compress_mt(src, n, cmp, &capm, mtLevels[k], w);
				checks++;
				if (cm != c1 || memcmp(cmp, one, c1)) fail("wzip/mt", name, mtLevels[k], "threads changed the stream");
				if (!guard_ok(cmp, bound)) fail("wzip/mt", name, mtLevels[k], "encoder wrote past its capacity");
			}
		}
		free(one);
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

/* WZIP_L with a dictionary: the first third of the data precedes the rest, either right before it in the same buffer
   or in a buffer of its own; every level, both decoders, into a buffer of its own and (encoded with the dictionary
   just before the input) right after the dictionary (a match may start in the dictionary and run on into the input) */
static void test_wzipl_dict(const unsigned char* src, int n, const char* name)
{
	if (n < 3 * 16384) return;                            /* WZIP_L takes inputs of 32 KB and more */
	const int dictSize = n / 3, len = n - dictSize;
	const int bound = WZIP_Cap_CmprSize(len);
	unsigned char* cmp = guarded(bound);
	unsigned char* dec = guarded(len);
	unsigned char* own = guarded(dictSize);
	memcpy(own, src, dictSize);
	unsigned char* t = (unsigned char*)malloc((size_t)bound + WZIP_TRUSTED_SRC_PAD);
	unsigned char* joint = guarded(n);                    /* the dictionary, then the output */
	for (int sep = 0; sep <= 1; sep++) {
		const unsigned char* const dict = sep ? own : src;
		const char* const what = sep ? "wzip_l/dict" : "wzip_l/prefix";
		for (int level = 0; level <= 13; level++) {
			if (level >= 7 && len > (1 << 18) && level != 11) continue;   /* the slow levels: enough on smaller inputs */
			WZIP_State_Str* s = WZIP_New_State_L(level, len, dict, dictSize);
			const int c = s ? WZIP_Compress_L(s, src + dictSize, len, cmp, bound) : -1;
			WZIP_Free_State(s);
			checks++;
			if (c < 0) { fail(what, name, level, "no state"); continue; }
			if (!guard_ok(cmp, bound)) fail(what, name, level, "encoder wrote past its capacity");
			if (c == 0) continue;                         /* did not fit: a caller would store the input */
			if (level == 7 || level == 11) {              /* with threads, the same stream */
				WZIP_State_Str* sm = WZIP_New_State_L(level, len, dict, dictSize);
				WZIP_Set_Workers(sm, level == 7 ? 4 : 6);   /* 6: the tree in 3 parts */
				const int cm = sm ? WZIP_Compress_L(sm, src + dictSize, len, t, bound) : -1;
				WZIP_Free_State(sm);
				if (cm != c || memcmp(t, cmp, c)) fail(what, name, level, "threads changed the stream");
			}
			memset(dec, 0, len);
			int d = WZIP_Decompress_L(cmp, c, dec, len, (void*)dict, dictSize);
			if (d != len || memcmp(src + dictSize, dec, len)) fail(what, name, level, "decoded data differs");
			if (!guard_ok(dec, len)) fail(what, name, level, "decoder wrote past the decoded size");
			if (!guard_ok(own, dictSize)) fail(what, name, level, "the dictionary was written to");
			memcpy(t, cmp, c);
			memset(t + c, 0, WZIP_TRUSTED_SRC_PAD);
			memset(dec, 0, len);
			d = WZIP_Decompress_L_Trusted(t, c, dec, len, (void*)dict, dictSize);
			if (d != len || memcmp(src + dictSize, dec, len)) fail(what, name, level, "trusted mode differs");
			if (!guard_ok(dec, len)) fail(what, name, level, "trusted mode wrote past the decoded size");
			for (int trusted = 0; trusted <= 1; trusted++) {  /* into the same buffer, right after the dictionary */
				memcpy(joint, src, dictSize);
				memset(joint + dictSize, 0, len);
				d = trusted ? WZIP_Decompress_L_Trusted(t, c, joint + dictSize, len, joint, dictSize)
				            : WZIP_Decompress_L(cmp, c, joint + dictSize, len, joint, dictSize);
				if (d != n - dictSize || memcmp(src, joint, n)) fail(what, name, level, trusted ? "trusted, after its dictionary, differs" : "after its dictionary, differs");
				if (!guard_ok(joint, n)) fail(what, name, level, "decoder wrote past the output after its dictionary");
			}
		}
	}
	free(cmp); free(dec); free(own); free(t); free(joint);
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
		memset(dec + n, 0xA5, WLZ_MEM_OVERHEAD);          /* the checked decoder needs no room past the output */
		const unsigned d = WLZ_Decompress((const char*)cmp, (char*)dec, c, (unsigned)n);
		if (d != (unsigned)n || memcmp(src, dec, n)) fail("wlz4", name, mode, "decoded data differs");
		for (int k = 0; k < WLZ_MEM_OVERHEAD; k++)
			if (dec[n + k] != 0xA5) { fail("wlz4", name, mode, "decoder wrote past the decoded size"); break; }
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
		if (bi && n > 0) {                                /* blocks compressed 4 at a time: the same frame */
			WZF_params q = p;
			q.nbWorkers = 4;
			unsigned char* g = (unsigned char*)malloc(bound);
			const size_t gs = WZF_compress(g, bound, src, (size_t)n, &q);
			if (gs != fs || memcmp(f, g, fs)) fail(what, name, level, "threads changed the frame");
			free(g);
		}
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

/* WZ frames of linked blocks: the same frame from any number of threads and from the streaming compressor fed whole
   blocks, decoded in one call (in place) and block by block (through the window); irregular blocks too */
static void test_linked(const unsigned char* src, int n, const char* name)
{
	static const int sets[][3] = { { 1, 15, 10 }, { 5, 16, 17 }, { 11, 16, 17 }, { 1, 17, 20 }, { 3, 15, 27 } };
	if (n == 0) {                                         /* linked blocks are WZIP's, at most 2^30 bytes each */
		const WZF_params bad[3] = { { WZF_CODEC_WLZ4, 1, 0, 0, 1, 20 }, { WZF_CODEC_WZIP, 1, 31, 0, 1, 20 },
		                            { WZF_CODEC_WZIP, 1, 0, 0, 1, -2 } };
		unsigned char f[64];
		for (int k = 0; k < 3; k++)
			if (WZF_getErrorCode(WZF_compress(f, sizeof f, f, 0, &bad[k])) != WZF_error_parameter)
				fail("frame/linked", "parameters", k, "accepted invalid parameters");
		checks++;
	}
	if (n < 1000) return;
	{   /* by default (windowLog 0) blocks are linked only when the threads outnumber those one block uses (levels 0-6:
	       1, 7-13: 7), for content over 64 MiB or of unknown size, and never at level 0 or with -1; the frame decodes
	       either way. { level, threads, windowLog, size known, linked } */
		static const int cases[7][5] = { { 2, 1, 0, 0, 0 }, { 2, 2, 0, 0, 1 }, { 2, 8, 0, 1, 0 }, { 2, 8, -1, 0, 0 },
		                                  { 0, 8, 0, 0, 0 }, { 7, 7, 0, 0, 0 }, { 7, 8, 0, 0, 1 } };
		for (int k = 0; k < 7; k++) {
			if (cases[k][0] >= 7 && n > (1 << 18)) continue;
			const WZF_params p = { WZF_CODEC_WZIP, cases[k][0], 0, k & 1, cases[k][1], cases[k][2] };
			const unsigned long long cs = cases[k][3] ? (unsigned long long)n : WZF_CONTENTSIZE_UNKNOWN;
			WZF_CCtx* c = WZF_createCCtx();
			const size_t cap = WZF_compressBound((size_t)n, &p) + 64;
			unsigned char* f = guarded(cap);
			unsigned char* dec = guarded(n);
			size_t pos = WZF_compressBegin(c, f, cap, &p, cs);
			const int linked = !WZF_isError(pos) && (f[4] & 0x10) != 0;
			if (!WZF_isError(pos)) { const size_t r = WZF_compressBlocks(c, f + pos, cap - pos, src, (size_t)n); pos = WZF_isError(r) ? r : pos + r; }
			if (!WZF_isError(pos)) { const size_t r = WZF_compressEnd(c, f + pos, cap - pos); pos = WZF_isError(r) ? r : pos + r; }
			checks++;
			if (WZF_isError(pos)) fail("frame/default", name, k, WZF_getErrorName(pos));
			else {
				if (linked != cases[k][4]) fail("frame/default", name, k, "linked blocks chosen wrongly");
				const size_t d = WZF_decompress(dec, (size_t)n, f, pos);
				if (d != (size_t)n || memcmp(src, dec, n)) fail("frame/default", name, k, "decoded data differs");
			}
			WZF_freeCCtx(c);
			free(f); free(dec);
		}
	}
	for (int k = 0; k < 5; k++) {
		const int level = sets[k][0];
		if (level > 5 && n > (1 << 18)) continue;
		WZF_params p = { WZF_CODEC_WZIP, level, sets[k][1], k & 1, 1, sets[k][2] };
		const size_t bound = WZF_compressBound((size_t)n, &p);
		unsigned char* f = guarded(bound);
		unsigned char* g = guarded(bound);
		unsigned char* dec = guarded(n);
		const char* what = "frame/linked";
		const size_t fs = WZF_compress(f, bound, src, (size_t)n, &p);
		checks++;
		if (WZF_isError(fs)) { fail(what, name, level, WZF_getErrorName(fs)); free(f); free(g); free(dec); continue; }
		if (!guard_ok(f, bound)) fail(what, name, level, "encoder wrote past its capacity");
		for (int t = 3; t <= 9; t += 6) {                 /* 3 threads: blocks 3 at a time; 9: 3 threads per block */
			p.nbWorkers = t;
			const size_t gs = WZF_compress(g, bound, src, (size_t)n, &p);
			if (gs != fs || memcmp(f, g, fs)) fail(what, name, level, "threads changed the frame");
		}
		{   /* the streaming compressor, three blocks per call, then the rest */
			WZF_CCtx* c = WZF_createCCtx();
			const size_t blk = (size_t)1 << p.blockLog;
			size_t pos = WZF_compressBegin(c, g, bound, &p, (unsigned long long)n);
			for (size_t done = 0; !WZF_isError(pos) && done < (size_t)n; ) {
				const size_t len = (size_t)n - done < 3 * blk ? (size_t)n - done : 3 * blk;
				const size_t r = WZF_compressBlocks(c, g + pos, bound - pos, src + done, len);
				pos = WZF_isError(r) ? r : pos + r;
				done += len;
			}
			if (!WZF_isError(pos)) { const size_t r = WZF_compressEnd(c, g + pos, bound - pos); pos = WZF_isError(r) ? r : pos + r; }
			if (pos != fs || memcmp(f, g, fs)) fail(what, name, level, "the streaming compressor made another frame");
			WZF_freeCCtx(c);
		}
		size_t d = WZF_decompress(dec, (size_t)n, f, fs);
		if (d != (size_t)n || memcmp(src, dec, n)) fail(what, name, level, WZF_isError(d) ? WZF_getErrorName(d) : "decoded data differs");
		if (!guard_ok(dec, n)) fail(what, name, level, "decoder wrote past the content size");
		memset(dec, 0, n);
		d = stream_decode(f, fs, dec, (size_t)n);
		if (d != (size_t)n || memcmp(src, dec, n)) fail(what, name, level, "block-by-block decoding differs");
		if (k == 0) {                                     /* WLZ4, a window log of 28, a block size log of 31: refused */
			static const int at[3] = { 4, 7, 6 }, v[3] = { 0x11, 28, 31 };
			for (int e = 0; e < 3; e++) {
				const unsigned char keep = f[at[e]];
				f[at[e]] = (unsigned char)(e ? v[e] : (keep | v[e]));
				if (WZF_getErrorCode(WZF_decompress(dec, (size_t)n, f, fs)) != WZF_error_unsupported)
					fail(what, name, e, "accepted an unsupported header");
				f[at[e]] = keep;
			}
		}
		free(f); free(g); free(dec);
	}
	{   /* irregular blocks of 1-100000 bytes, content size unknown, block by block both ways */
		WZF_params p = { WZF_CODEC_WZIP, 2, 17, 0, 1, 16 };
		WZF_CCtx* c = WZF_createCCtx();
		const size_t cap = WZF_compressBound((size_t)n, &p) + (size_t)n / 1000 * WZF_BLOCK_HEADER + 64;
		unsigned char* f = guarded(cap);
		unsigned char* dec = guarded(n);
		size_t pos = WZF_compressBegin(c, f, cap, &p, WZF_CONTENTSIZE_UNKNOWN);
		for (int done = 0, k = 0; !WZF_isError(pos) && done < n; k++) {
			const int len = n - done < 1 + (k * 40009) % 100000 ? n - done : 1 + (k * 40009) % 100000;
			const size_t r = WZF_compressBlock(c, f + pos, cap - pos, src + done, len);
			pos = WZF_isError(r) ? r : pos + r;
			done += len;
		}
		if (!WZF_isError(pos)) { const size_t r = WZF_compressEnd(c, f + pos, cap - pos); pos = WZF_isError(r) ? r : pos + r; }
		checks++;
		if (WZF_isError(pos)) fail("frame/linked-stream", name, 2, WZF_getErrorName(pos));
		else {
			size_t d = WZF_decompress(dec, (size_t)n, f, pos);
			if (d != (size_t)n || memcmp(src, dec, n)) fail("frame/linked-stream", name, 2, "decoded data differs");
			memset(dec, 0, n);
			d = stream_decode(f, pos, dec, (size_t)n);
			if (d != (size_t)n || memcmp(src, dec, n)) fail("frame/linked-stream", name, 2, "block-by-block decoding differs");
		}
		WZF_freeCCtx(c);
		free(f); free(dec);
	}
}

static void test_all(const unsigned char* src, int n, const char* name)
{
	const int before = failures;
	test_wzip(src, n, name);
	test_wzipl_dict(src, n, name);
	if (n > 0) test_wzips(src, n, name);
	test_wlz4(src, n, name);
	test_frames(src, n, name);
	test_linked(src, n, name);
	printf("%-28s %10d bytes  %s\n", name, n, failures == before ? "ok" : "FAILED");
	fflush(stdout);
}

static unsigned rng = 12345;
static unsigned next_rand(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

/* bytes without any repeat (xorshift64*: unlike the low bits of next_rand, no short period) */
static unsigned long long xs = 88172645463325252ull;
static unsigned char next_byte(void) { xs ^= xs >> 12; xs ^= xs << 25; xs ^= xs >> 27; return (unsigned char)((xs * 2685821657736338717ull) >> 56); }

/* text-like data: random words */
static void fill_words(unsigned char* b, int n)
{
	static const char words[][8] = { "the ", "match ", "window ", "length ", "offset ", "of ", "a ", "and ", "LZ77 ", "code " };
	for (int p = 0; p < n; ) {
		const char* w = words[next_rand() % 10];
		for (int k = 0; w[k] && p < n; k++) b[p++] = (unsigned char)w[k];
	}
}

/* little-endian 32-bit counters: no 6 bytes repeat, so level 0 finds no match, yet half the bytes are zero */
static void fill_counters(unsigned char* b, int n)
{
	for (int i = 0; i + 4 <= n; i += 4) {
		b[i] = (unsigned char)(i >> 2); b[i + 1] = (unsigned char)(i >> 10); b[i + 2] = (unsigned char)(i >> 18); b[i + 3] = 0;
	}
}

/* A stretch without any match longer than a literal run can be (2^24), then text: levels 0 and 1 must still compress
   the text (1.0.0 stored the whole input), and the stream must decode. */
static void test_long_stretch(void)
{
	const int nr = (1 << 24) + (1 << 20), nt = 1 << 20, n = nr + nt;
	unsigned char* b = (unsigned char*)malloc(n);
	const int bound = WZIP_Cap_CmprSize(n);
	unsigned char* cmp = (unsigned char*)malloc(bound);
	unsigned char* dec = (unsigned char*)malloc(n);
	if (!b || !cmp || !dec) { printf("out of memory\n"); exit(2); }
	for (int i = 0; i < nr; i++) b[i] = next_byte();
	fill_words(b + nr, nt);
	for (int level = 0; level <= 1; level++) {
		int cap = bound, dcap = n;
		const int c = wzip_compress(b, n, cmp, &cap, level);
		checks++;
		if (c <= 0 || c > nr + nt / 2) fail("wzip", "17 MiB random + 1 MiB text", level, "the text after the random stretch was not compressed");
		else if (wzip_decompress(cmp, c, dec, &dcap) != n || memcmp(b, dec, n)) fail("wzip", "17 MiB random + 1 MiB text", level, "decoded data differs");
	}
	printf("%-28s %10d bytes  %s\n", "random 17 MiB + text 1 MiB", n, "done");
	free(b); free(cmp); free(dec);
}

/* The encoders' output must be the same on every platform (qsort's ties, log2 and floating point once made it differ):
   a hash of the streams of every level for a few inputs, against the value on x86-64 Linux. 64-bit little-endian
   builds only: 32-bit builds compare 4-byte words in the match finders, and so find other matches. */
static unsigned fnv1a(unsigned h, const unsigned char* p, int n)
{
	for (int i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
	return h;
}

static void test_same_output(void)
{
	const unsigned one = 1;
#ifdef WZIP_TEST_MAX_OFF_WIDTH
	if (one) { printf("%-28s (not with narrowed windows)\n", "same output everywhere"); return; }
#endif
	if (sizeof(void*) != 8 || *(const unsigned char*)&one != 1) { printf("%-28s (64-bit little-endian only)\n", "same output everywhere"); return; }
	static const struct { const char* name; int n; unsigned expect; } cases[] = {
		{ "counters 256 KiB", 1 << 18, 0x5BE5F4B2u }, { "words 256 KiB", 1 << 18, 0xAF6A75CDu }, { "words 16 KiB", 1 << 14, 0xA8DC538Fu },
	};
	const int n = 1 << 18;
	unsigned char* b = (unsigned char*)malloc(n);
	const int bound = WZIP_Cap_CmprSize(n);
	unsigned char* cmp = (unsigned char*)malloc(bound);
	if (!b || !cmp) { printf("out of memory\n"); exit(2); }
	for (int k = 0; k < 3; k++) {
		const unsigned saved = rng;
		rng = 777;                                       /* the same words whatever ran before */
		if (k == 0) fill_counters(b, n); else fill_words(b, n);
		rng = saved;
		unsigned h = 2166136261u;
		for (int level = 0; level <= 13; level++) {
			int cap = bound;
			const int c = wzip_compress(b, cases[k].n, cmp, &cap, level);
			h = fnv1a(h, cmp, c > 0 ? c : 0);
		}
		checks++;
		if (h != cases[k].expect) {
			char why[96];
			snprintf(why, sizeof why, "stream hash %08X, expected %08X", h, cases[k].expect);
			fail("wzip", cases[k].name, -1, why);
		}
	}
	printf("%-28s %10s        %s\n", "same output everywhere", "", "done");
	free(b); free(cmp);
}

static void synthetic(void)
{
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
	fill_counters(b, n);                                 /* literal counts that fill their last block exactly */
	test_all(b, 32768, "counters 32 KiB");
	test_all(b, 1 << 18, "counters 256 KiB");
	fill_words(b, n);                                    /* text-like: random words */
	for (int size = 1000; size <= n; size *= 4) {
		snprintf(name, sizeof name, "words %d", size);
		test_all(b, size, name);
	}
	for (int i = 0; i < n; i++)                          /* long runs with literal bursts */
		b[i] = (unsigned char)((i / 1000) & 1 ? next_rand() : (i / 2000) & 0xFF);
	test_all(b, n, "runs and bursts 1 MB");
	free(b);
	test_long_stretch();
	test_same_output();
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
