/*
 * libFuzzer target: compress the input with a codec and level chosen by its first two bytes (WZIP_L also with the
 * input's first part as its dictionary, and WZ frames of linked blocks), decode it, and require the original back;
 * the encoders must stay inside their buffers and their stated bounds.
 * Copyright (c) 2026-present, Yingquan (Cody) Wu. SPDX-License-Identifier: BSD-2-Clause
 * Build and run: tests/fuzz/run.sh (clang -fsanitize=fuzzer,address,undefined, with the sources)
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "WZIP.h"
#include "WLZ4.h"
#include "wzframe.h"

/* a copy in a buffer of exactly its size, so that a read past it is caught */
static unsigned char* exact(const void* data, size_t size)
{
	unsigned char* p = (unsigned char*)malloc(size ? size : 1);
	if (size) memcpy(p, data, size);
	return p;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	if (size < 2) return 0;
	const unsigned sel = data[0], lv = data[1];
	const size_t n = size - 2;
	unsigned char* in = exact(data + 2, n);
	unsigned char* out = (unsigned char*)malloc(n ? n : 1);
	switch (sel % 7) {
	case 0: {                                           /* wzip_compress, levels 0-13 (the optimal ones on small inputs) */
		const int level = (int)(lv % 14);
		if (level >= 7 && n > 65536) break;
		int cap = WZIP_Cap_CmprSize((int)n);
		unsigned char* c = (unsigned char*)malloc(cap);
		const int cs = wzip_compress(in, (int)n, c, &cap, level);
		if (cs <= 0 || (size_t)cs > n + 2) abort();
		unsigned char* e = exact(c, cs);
		int dcap = (int)n;
		if (wzip_decompress(e, cs, out, &dcap) != (int)n || memcmp(in, out, n)) abort();
		free(e); free(c);
		break;
	}
	case 1: case 2: {                                   /* WLZ4: fast (acceleration 1-16), lazy, levels 0-12 */
		const int mode = (int)(lv % 15) - 2;
		const unsigned bound = WLZ_COMPRESSBOUND((unsigned)n);
		unsigned char* c = (unsigned char*)malloc(bound);
		unsigned cs;
		if (mode < 0) {
			WLZ_State_Str* s = WLZ_New_State();
			cs = mode == -2 ? WLZ_Compress_Fast(s, (const char*)in, (char*)c, (unsigned)n, bound, 1 + (int)(lv >> 4))
			                : WLZ_Compress(s, (const char*)in, (char*)c, (unsigned)n, bound);
			WLZ_Free_State(s);
		}
		else {
			WLZhc_State_Str* s = WLZhc_New_State();
			cs = WLZhc_Compress(s, (const char*)in, (char*)c, (unsigned)n, bound, mode);
			WLZhc_Free_State(s);
		}
		if (cs == 0 || cs > n + 15) abort();
		unsigned char* e = exact(c, cs);
		if (WLZ_Decompress((const char*)e, (char*)out, cs, (unsigned)n) != n || memcmp(in, out, n)) abort();
		free(e); free(c);
		break;
	}
	case 3: {                                           /* WZIP_S, levels 1-9, blocks of up to 32 KiB */
		if (n == 0 || n > WZIPS_MAX_BLOCK) break;
		WZIPS_CCtx* cctx = WZIPS_createCCtx();
		const int bound = WZIPS_COMPRESSBOUND((int)n);
		unsigned char* c = (unsigned char*)malloc(bound);
		const int cs = WZIPS_compress(cctx, in, (int)n, c, bound, 1 + (int)(lv % 9));
		if (cs <= 0 || cs > bound) abort();
		unsigned char* e = exact(c, cs);
		if (WZIPS_decompress(e, cs, out, (int)n) != (int)n || memcmp(in, out, n)) abort();
		free(e); free(c);
		WZIPS_freeCCtx(cctx);
		break;
	}
	case 4: {                                           /* WZ frames of both codecs, blocks of 1-8 KiB */
		const int codec = (int)(lv & 1);
		const WZF_params p = { codec, codec ? (int)((lv >> 1) % 15) - 2 : (int)((lv >> 1) % 7), 10 + (int)((lv >> 4) % 4),
		                       (int)((lv >> 6) & 1) };
		const size_t bound = WZF_compressBound(n, &p);
		unsigned char* f = (unsigned char*)malloc(bound);
		const size_t fs = WZF_compress(f, bound, in, n, &p);
		if (WZF_isError(fs)) abort();
		unsigned char* e = exact(f, fs);
		if (WZF_decompress(out, n, e, fs) != n || memcmp(in, out, n)) abort();
		free(e); free(f);
		break;
	}
	case 5: {                                           /* WZIP_L with a dictionary: the input's first part, in a buffer
		                                                   of its own or (bit 4 of the level byte) right before the rest */
		const size_t dn = n / 4 + (lv & 15) * 977 % (n / 4 + 1);
		if (n - dn < 32768 || n > 262144) break;
		const int level = (int)(lv % 14), len = (int)(n - dn), prefix = (lv >> 4) & 1;
		unsigned char* dict = prefix ? in : exact(in, dn);
		unsigned char* body = prefix ? in + dn : exact(in + dn, len);
		const int cap = WZIP_Cap_CmprSize(len);
		unsigned char* c = (unsigned char*)malloc(cap);
		WZIP_State_Str* s = WZIP_New_State_L(level, len, dict, (int)dn);
		const int cs = s ? WZIP_Compress_L(s, body, len, c, cap) : -1;
		WZIP_Free_State(s);
		if (cs < 0 || cs > cap) abort();
		if (cs > 0) {                                   /* 0: did not fit, a caller stores the input */
			unsigned char* e = exact(c, cs);
			if (prefix) {                               /* decoded right after a copy of the dictionary */
				memcpy(out, in, dn);
				if (WZIP_Decompress_L(e, cs, out + dn, len, out, (int)dn) != len || memcmp(in, out, n)) abort();
			}
			else if (WZIP_Decompress_L(e, cs, out, len, dict, (int)dn) != len || memcmp(body, out, len)) abort();
			free(e);
		}
		free(c);
		if (!prefix) { free(dict); free(body); }
		break;
	}
	case 6: {                                           /* WZ frames of linked WZIP blocks of 32 or 64 KiB, windows of
		                                                   1 KiB to 128 MiB; with 3 threads the same frame (MT builds),
		                                                   decoded in one call and block by block (through the window) */
		const int level = (int)(lv % 14);
		if (level >= 7 && n > 131072) break;
		WZF_params p = { WZF_CODEC_WZIP, level, 15 + (int)((lv >> 4) & 1), (int)((lv >> 5) & 1), 1, 10 + (int)(sel / 7 % 18) };
		const size_t bound = WZF_compressBound(n, &p);
		unsigned char* f = (unsigned char*)malloc(bound);
		const size_t fs = WZF_compress(f, bound, in, n, &p);
		if (WZF_isError(fs)) abort();
#if WZIP_MULTITHREAD
		p.nbWorkers = 3;
		unsigned char* g = (unsigned char*)malloc(bound);
		if (WZF_compress(g, bound, in, n, &p) != fs || memcmp(f, g, fs)) abort();
		free(g);
#endif
		unsigned char* e = exact(f, fs);
		if (WZF_decompress(out, n, e, fs) != n || memcmp(in, out, n)) abort();
		WZF_DCtx* d = WZF_createDCtx();
		size_t pos = WZF_decompressBegin(d, e, fs), o = 0;
		if (WZF_isError(pos)) abort();
		for (;;) {
			int raw;
			const size_t c = WZF_nextBlock(d, e + pos, &raw);
			if (WZF_isError(c)) abort();
			pos += WZF_BLOCK_HEADER;
			if (c == 0) break;
			const size_t r = WZF_decompressBlock(d, out + o, n - o, e + pos, c);
			if (WZF_isError(r)) abort();
			o += r; pos += c;
		}
		if (o != n || memcmp(in, out, n) || WZF_isError(WZF_decompressEnd(d, e + pos, WZF_endSize(d)))) abort();
		WZF_freeDCtx(d);
		free(e); free(f);
		break;
	}
	}
	free(in); free(out);
	return 0;
}
