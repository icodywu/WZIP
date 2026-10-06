/*
 * libFuzzer target: one decoder on arbitrary input, selected by FUZZ_TARGET at compile time
 *   0 WZ frames (WZF_decompress, then block by block)   1 wzip_decompress   2 WLZ_Decompress (exact capacity)
 *   3 WLZ_Decompress_wDict   4 WZIPS_decompress   5 WZIPS_decompress_usingDict
 * Copyright (c) 2026-present, Yingquan (Cody) Wu. SPDX-License-Identifier: BSD-2-Clause
 * Build and run: tests/fuzz/run.sh (clang -fsanitize=fuzzer,address,undefined -DFUZZ_TARGET=k, with the sources)
 * A decoder may reject the input or return output, but must not read or write outside its buffers (the sanitizers
 * check) and must agree with itself: a stream it accepts decodes to the size its header states.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "WZIP.h"
#include "WLZ4.h"
#include "wzframe.h"

#ifndef FUZZ_TARGET
#  define FUZZ_TARGET 0
#endif
#define MAX_OUT (1u << 22)                              /* larger stated sizes are not decoded */

static const char dictText[] = "the window of a match of length l: short matches near, long ones far; "
	"WZIP and WLZ4, LZ77 with match-length-dependent sliding windows";

static unsigned char* exact(const void* data, size_t size)      /* a copy in a buffer of exactly its size */
{
	unsigned char* p = (unsigned char*)malloc(size ? size : 1);
	if (size) memcpy(p, data, size);
	return p;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	unsigned char* in = exact(data, size);
#if FUZZ_TARGET == 0
	const unsigned long long cs = WZF_getContentSize(in, size);
	const size_t cap = cs != WZF_CONTENTSIZE_UNKNOWN && cs < MAX_OUT ? (size_t)cs : MAX_OUT / 4;
	unsigned char* out = (unsigned char*)malloc(cap ? cap : 1);
	const size_t r = WZF_decompress(out, cap, in, size);
	if (!WZF_isError(r) && cs != WZF_CONTENTSIZE_UNKNOWN && r != cs) abort();     /* accepted: the stated size */
	free(out);
	{   /* block by block, as a streaming caller would */
		WZF_DCtx* d = WZF_createDCtx();
		const size_t h = WZF_decompressBegin(d, in, size);
		if (!WZF_isError(h) && h <= size && !WZF_frameHeader(d)->skippable) {
			size_t pos = h;
			while (size - pos >= WZF_BLOCK_HEADER) {
				int raw;
				const size_t c = WZF_nextBlock(d, in + pos, &raw);
				if (WZF_isError(c) || c == 0) break;
				pos += WZF_BLOCK_HEADER;
				if (size - pos < c) break;
				const size_t ds = WZF_blockDecodedSize(d, in + pos, c);
				if (WZF_isError(ds) || ds > MAX_OUT) break;
				unsigned char* b = (unsigned char*)malloc(ds ? ds : 1);
				const size_t got = WZF_decompressBlock(d, b, ds, in + pos, c);
				free(b);
				if (WZF_isError(got)) break;
				if (got != ds) abort();                 /* a block decodes to the size it announces */
				pos += c;
			}
		}
		WZF_freeDCtx(d);
	}
#elif FUZZ_TARGET == 1
	int left = (int)(size > 0x7FFFFFFF ? 0x7FFFFFFF : size);
	const int ds = WZIP_Read_DecSize(in, &left);
	const int n = ds ? ds : left;
	if (n >= 0 && (unsigned)n <= MAX_OUT) {
		unsigned char* out = (unsigned char*)malloc(n ? n : 1);
		int cap = n;
		const int r = wzip_decompress(in, (int)size, out, &cap);
		if (r != 0 && r != n) abort();
		free(out);
	}
#elif FUZZ_TARGET == 2 || FUZZ_TARGET == 3
	const unsigned n = WLZ_Read_DecSize((const char*)in, (unsigned)size);
	if (n <= MAX_OUT) {
		unsigned char* out = (unsigned char*)malloc(n ? n : 1);
#  if FUZZ_TARGET == 2
		const unsigned r = WLZ_Decompress((const char*)in, (char*)out, (unsigned)size, n);
#  else
		unsigned char* dd = exact(dictText, sizeof dictText - 1);
		const unsigned r = WLZ_Decompress_wDict((const char*)in, (char*)out, (unsigned)size, n, (const char*)dd,
		                                        sizeof dictText - 1);
		free(dd);
#  endif
		if (r != 0 && r != n) abort();
		free(out);
	}
#elif FUZZ_TARGET == 4 || FUZZ_TARGET == 5
	const int n = WZIPS_getDecompressedSize(in, (int)size);
	if (n > 0 && n <= WZIPS_MAX_BLOCK) {
		unsigned char* out = (unsigned char*)malloc(n);
#  if FUZZ_TARGET == 4
		const int r = WZIPS_decompress(in, (int)size, out, n);
#  else
		unsigned char* dd = exact(dictText, sizeof dictText - 1);
		const int r = WZIPS_decompress_usingDict(in, (int)size, out, n, dd, (int)(sizeof dictText - 1));
		free(dd);
#  endif
		if (r > 0 && r != n) abort();
		free(out);
	}
#endif
	free(in);
	return 0;
}
