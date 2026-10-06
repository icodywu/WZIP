/*
 * libFuzzer target: compress the input with a codec and level chosen by its first two bytes (WZIP_L also with the
 * input's first part as its dictionary), decode it, and require the original back; the encoders must stay inside
 * their buffers and their stated bounds.
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
	switch (sel % 6) {
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
	case 5: {                                           /* WZIP_L with a dictionary: the input's first part, own buffer */
		const size_t dn = n / 4 + (lv & 15) * 977 % (n / 4 + 1);
		if (n - dn < 32768 || n > 262144) break;
		const int level = (int)(lv % 14), len = (int)(n - dn);
		unsigned char* dict = exact(in, dn);
		unsigned char* body = exact(in + dn, len);
		const int cap = WZIP_Cap_CmprSize(len);
		unsigned char* c = (unsigned char*)malloc(cap);
		WZIP_State_Str* s = WZIP_New_State_L(level, len, dict, (int)dn);
		const int cs = s ? WZIP_Compress_L(s, body, len, c, cap) : -1;
		WZIP_Free_State(s);
		if (cs < 0 || cs > cap) abort();
		if (cs > 0) {                                   /* 0: did not fit, a caller stores the input */
			unsigned char* e = exact(c, cs);
			if (WZIP_Decompress_L(e, cs, out, len, dict, (int)dn) != len || memcmp(body, out, len)) abort();
			free(e);
		}
		free(c); free(dict); free(body);
		break;
	}
	}
	free(in); free(out);
	return 0;
}
