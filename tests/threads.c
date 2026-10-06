/*
 * Thread-safety test: several threads compress and decompress at once, with WZIP (L and M), WZIP_S, WLZ4 and WZ
 * frames, each with its own contexts, and every output must equal the one a single thread produces.
 * Copyright (c) 2026-present, Yingquan (Cody) Wu. SPDX-License-Identifier: BSD-2-Clause
 *
 * usage: threads [threads [rounds]]      (default 8 threads, 3 rounds; POSIX threads, or Windows threads)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "WZIP.h"
#include "WLZ4.h"
#include "wzframe.h"

#ifdef _WIN32
#  include <windows.h>
typedef HANDLE thread_t;
static DWORD WINAPI thread_main(LPVOID arg);
static int  thread_start(thread_t* t, void* arg) { *t = CreateThread(NULL, 0, thread_main, arg, 0, NULL); return *t == NULL; }
static void thread_join(thread_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
#  include <pthread.h>
typedef pthread_t thread_t;
static void* thread_main(void* arg);
static int  thread_start(thread_t* t, void* arg) { return pthread_create(t, NULL, thread_main, arg); }
static void thread_join(thread_t t) { pthread_join(t, NULL); }
#endif

#define N_INPUTS 6
static unsigned char* input[N_INPUTS];
static int inputSize[N_INPUTS];
static unsigned expected[N_INPUTS][4];        /* XXH32 of the single-threaded outputs: WZIP, WZIP_S, WLZ4, frame */
static int rounds;
static volatile int failures;

static const int wzipLevel[N_INPUTS] = { 1, 5, 9, 11, 3, 13 };

/* compresses input k with every codec, decodes each stream, and returns the hashes of the compressed streams */
static int run(int k, unsigned out[4])
{
	const unsigned char* const src = input[k];
	const int n = inputSize[k];
	int ok = 1;
	unsigned char* c = (unsigned char*)malloc((size_t)n + 1024 + n / 8);
	unsigned char* d = (unsigned char*)malloc((size_t)n + 64);
	{   /* WZIP: L or M by size */
		int cap = WZIP_Cap_CmprSize(n), dcap = n;
		const int cs = wzip_compress(src, n, c, &cap, wzipLevel[k]);
		out[0] = WZF_XXH32(c, (size_t)cs, 0);
		ok &= cs > 0 && wzip_decompress(c, cs, d, &dcap) == n && !memcmp(src, d, n);
	}
	{   /* WZIP_S: 4 KB blocks */
		WZIPS_CCtx* cctx = WZIPS_createCCtx();
		unsigned h = 0;
		for (int pos = 0; pos < n; pos += 4096) {
			const int len = n - pos < 4096 ? n - pos : 4096;
			const int cs = WZIPS_compress(cctx, src + pos, len, c, WZIPS_COMPRESSBOUND(len), 1 + k % 9);
			h = h * 31 + WZF_XXH32(c, (size_t)cs, 0);
			ok &= cs > 0 && WZIPS_decompress(c, cs, d, len) == len && !memcmp(src + pos, d, len);
		}
		WZIPS_freeCCtx(cctx);
		out[1] = h;
	}
	{   /* WLZ4: an optimal level */
		WLZhc_State_Str* hs = WLZhc_New_State();
		const unsigned cs = WLZhc_Compress(hs, (const char*)src, (char*)c, (unsigned)n, WLZ_COMPRESSBOUND((unsigned)n), 8 + k % 5);
		out[2] = WZF_XXH32(c, cs, 0);
		ok &= cs > 0 && WLZ_Decompress((const char*)c, (char*)d, cs, (unsigned)n + WLZ_MEM_OVERHEAD) == (unsigned)n && !memcmp(src, d, n);
		WLZhc_Free_State(hs);
	}
	{   /* a WZ frame of 64 KiB blocks */
		const WZF_params p = { k & 1, (k & 1) ? -1 : 2, 16, 0 };
		const size_t bound = WZF_compressBound((size_t)n, &p);
		unsigned char* f = (unsigned char*)malloc(bound);
		const size_t fs = WZF_compress(f, bound, src, (size_t)n, &p);
		out[3] = WZF_isError(fs) ? 0 : WZF_XXH32(f, fs, 0);
		ok &= !WZF_isError(fs) && WZF_decompress(d, (size_t)n, f, fs) == (size_t)n && !memcmp(src, d, n);
		free(f);
	}
	free(c); free(d);
	return ok;
}

static void worker(void* arg)
{
	const int id = (int)(size_t)arg;
	for (int r = 0; r < rounds; r++) {
		const int k = (id + r) % N_INPUTS;
		unsigned h[4];
		if (!run(k, h) || memcmp(h, expected[k], sizeof h)) {
			printf("FAIL thread %d round %d input %d: output differs from the single-threaded one\n", id, r, k);
			failures++;
		}
	}
}

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID arg) { worker(arg); return 0; }
#else
static void* thread_main(void* arg) { worker(arg); return NULL; }
#endif

static unsigned rng = 777;
static unsigned next_rand(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

int main(int argc, char** argv)
{
	static const char words[][8] = { "the ", "match ", "window ", "length ", "offset ", "of ", "a ", "and ", "LZ77 ", "code " };
	const int nThreads = argc > 1 ? atoi(argv[1]) : 8;
	rounds = argc > 2 ? atoi(argv[2]) : 3;
	static const int sizes[N_INPUTS] = { 20000, 70000, 300000, 1 << 20, 5000, 200000 };
	for (int k = 0; k < N_INPUTS; k++) {            /* text-like inputs of different sizes and statistics */
		inputSize[k] = sizes[k];
		input[k] = (unsigned char*)malloc(sizes[k]);
		for (int p = 0; p < sizes[k]; ) {
			const char* w = words[(next_rand() + k) % 10];
			for (int i = 0; w[i] && p < sizes[k]; i++) input[k][p++] = (unsigned char)(k & 1 && next_rand() % 50 == 0 ? next_rand() : w[i]);
		}
		if (!run(k, expected[k])) { printf("FAIL single-threaded round trip of input %d\n", k); return 1; }
	}
	thread_t* t = (thread_t*)malloc(nThreads * sizeof(thread_t));
	for (int i = 0; i < nThreads; i++)
		if (thread_start(&t[i], (void*)(size_t)i)) { printf("FAIL: cannot start a thread\n"); return 1; }
	for (int i = 0; i < nThreads; i++) thread_join(t[i]);
	printf("%d threads x %d rounds, %d failures\n", nThreads, rounds, failures);
	return failures ? 1 : 0;
}
