/*
 * Seed corpora for the fuzz targets: make_seeds <work directory> <file>...
 * Writes, for pieces of each file, a bare stream of every codec at a few levels into the corpus of the matching
 * decoder target, WZ frames into the frame target's, and inputs into the round-trip target's (with, from all the files
 * together, inputs for its WZIP_L dictionary case).
 * Copyright (c) 2026-present, Yingquan (Cody) Wu. SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "WZIP.h"
#include "WLZ4.h"
#include "wzframe.h"

static const char* work;

static void put(const char* target, const char* base, const char* tag, const void* p, size_t n)
{
	char name[1024];
	snprintf(name, sizeof name, "%s/%s/corpus/%s.%s", work, target, base, tag);
	FILE* f = fopen(name, "wb");
	if (!f) { perror(name); exit(1); }
	fwrite(p, 1, n, f);
	fclose(f);
}

int main(int argc, char** argv)
{
	static const int cuts[3] = { 300, 3000, 30000 };
	static const char dictText[] = "the window of a match of length l: short matches near, long ones far; "
		"WZIP and WLZ4, LZ77 with match-length-dependent sliding windows";
	work = argv[1];
	for (int a = 2; a < argc; a++) {
		FILE* f = fopen(argv[a], "rb");
		if (!f) { perror(argv[a]); return 1; }
		unsigned char* data = (unsigned char*)malloc(1 << 20);
		const size_t len = fread(data, 1, 1 << 20, f);
		fclose(f);
		const char* base = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
		unsigned char* c = (unsigned char*)malloc((1 << 20) + 4096);
		char tag[64];
		for (int k = 0; k < 3; k++) {
			const size_t n = cuts[k] < (int)len ? (size_t)cuts[k] : len;
			for (int level = 0; level <= 13; level += 3) {            /* WZIP: M below 32 KiB, L from it */
				int cap = WZIP_Cap_CmprSize((int)n);
				const int cs = wzip_compress(data, (int)n, c, &cap, level);
				snprintf(tag, sizeof tag, "%zu.L%d", n, level);
				if (cs > 0) put("wzip", base, tag, c, cs);
			}
			for (int mode = -2; mode <= 12; mode += 2) {               /* WLZ4 */
				unsigned cs;
				if (mode < 0) { WLZ_State_Str* s = WLZ_New_State(); cs = WLZ_Compress(s, (const char*)data, (char*)c, (unsigned)n, WLZ_COMPRESSBOUND((unsigned)n)); WLZ_Free_State(s); }
				else { WLZhc_State_Str* s = WLZhc_New_State(); cs = WLZhc_Compress(s, (const char*)data, (char*)c, (unsigned)n, WLZ_COMPRESSBOUND((unsigned)n), mode); WLZhc_Free_State(s); }
				snprintf(tag, sizeof tag, "%zu.L%d", n, mode);
				if (cs) { put("wlz4", base, tag, c, cs); put("wlz4dict", base, tag, c, cs); }
			}
			if (n <= WZIPS_MAX_BLOCK) {                                  /* WZIP_S, with and without the dictionary */
				WZIPS_CCtx* cctx = WZIPS_createCCtx();
				WZIPS_CDict* cd = WZIPS_createCDict(dictText, (int)sizeof dictText - 1);
				for (int level = 1; level <= 9; level += 4) {
					int cs = WZIPS_compress(cctx, data, (int)n, c, WZIPS_COMPRESSBOUND((int)n), level);
					snprintf(tag, sizeof tag, "%zu.L%d", n, level);
					if (cs > 0) put("wzips", base, tag, c, cs);
					cs = WZIPS_compress_usingCDict(cctx, cd, data, (int)n, c, WZIPS_COMPRESSBOUND((int)n), level);
					if (cs > 0) put("wzipsdict", base, tag, c, cs);
				}
				WZIPS_freeCDict(cd);
				WZIPS_freeCCtx(cctx);
			}
			for (int codec = 0; codec <= 1; codec++) {                   /* frames of 1 KiB blocks */
				const WZF_params p = { codec, codec ? 2 : 3, 10, 0 };
				const size_t cs = WZF_compress(c, (1 << 20) + 4096, data, n, &p);
				snprintf(tag, sizeof tag, "%zu.c%d.wz", n, codec);
				if (!WZF_isError(cs)) put("frame", base, tag, c, cs);
			}
			unsigned char* rt = (unsigned char*)malloc(n + 2);              /* round trips: selector, level, data */
			rt[0] = (unsigned char)k; rt[1] = (unsigned char)(3 * k + 1);
			memcpy(rt + 2, data, n);
			snprintf(tag, sizeof tag, "%zu.rt", n);
			put("roundtrip", base, tag, rt, n + 2);
			free(rt);
		}
		free(data); free(c);
	}
	{   /* round trips of WZIP_L with a dictionary (selector 5) need 64 KiB and more: all the files, repeated to 96 KiB */
		const size_t n = 96 << 10;
		unsigned char* rt = (unsigned char*)malloc(n + 2);
		size_t got = 0;
		while (got < n) {
			const size_t before = got;
			for (int a = 2; a < argc && got < n; a++) {
				FILE* f = fopen(argv[a], "rb");
				if (!f) continue;
				got += fread(rt + 2 + got, 1, n - got, f);
				fclose(f);
			}
			if (got == before) break;
		}
		for (int level = 1; got == n && level <= 13; level += 4) {
			char tag[64];
			rt[0] = 5; rt[1] = (unsigned char)level;
			snprintf(tag, sizeof tag, "dict.L%d.rt", level);
			put("roundtrip", "all", tag, rt, n + 2);
		}
		free(rt);
	}
	return 0;
}
