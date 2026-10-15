/*
 * WZ frames: the container of WZIP and WLZ4 data (doc/frame_format.md)
 * Copyright (c) 2026-present, Yingquan (Cody) Wu.
 * SPDX-License-Identifier: BSD-2-Clause (see LICENSE)
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "wzframe.h"
#include "WZIP.h"
#include "WLZ4.h"
#include "wz_threads.h"

#define WZF_FRAME_VERSION   0
#define WZF_CODEC_VERSION   1           /* the WZIP and WLZ4 formats of October 2026 */
#define WZF_CODEC_VERSION_2 2           /* WZIP with sized sequence blocks and filters (1.1.0, levels 2-13), which 1.0 cannot decode */
#define WZF_FLAG_CHECKSUM   0x04
#define WZF_FLAG_SIZE       0x08
#define WZF_FLAG_LINKED     0x10        /* linked blocks: a window log follows the block size log; WZIP only, b <= 30 */
#define WZF_FLAG_XXH64      0x20        /* the checksum is XXH64's low 32 bits; bits 5-6 are the checksum type */

unsigned WZF_versionNumber(void) { return WZF_VERSION_NUMBER; }
const char* WZF_versionString(void) { return WZF_VERSION_STRING; }
#if WZF_VERSION_NUMBER != WZIP_VERSION_NUMBER || WZF_VERSION_NUMBER != WLZ_VERSION_NUMBER
#  error "WZIP.h, WLZ4.h and wzframe.h must carry the same version"
#endif

/*------   Errors: (size_t)-code   ------*/
#define ERR(e) ((size_t)-(ptrdiff_t)(WZF_error_##e))
unsigned WZF_isError(size_t code) { return code > (size_t)-(ptrdiff_t)WZF_error_maxCode; }
WZF_ErrorCode WZF_getErrorCode(size_t code) { return WZF_isError(code) ? (WZF_ErrorCode)(0 - code) : WZF_error_none; }
const char* WZF_getErrorName(size_t code)
{
	static const char* const names[WZF_error_maxCode] = {
		"no error", "error", "invalid parameter", "out of memory", "output buffer too small",
		"input ends inside a frame", "not a WZ frame", "unsupported codec, format version or flag",
		"corrupted data", "checksum mismatch", "content size mismatch" };
	return names[WZF_getErrorCode(code)];
}

/*------   Little-endian fields   ------*/
/* one load each on a little-endian processor (the test folds to a constant), the bytes one by one otherwise */
static int little_endian(void) { const uint32_t one = 1; unsigned char b; memcpy(&b, &one, 1); return b; }
static uint32_t rd32(const unsigned char* p)
{
	uint32_t v;
	if (little_endian()) { memcpy(&v, p, 4); return v; }
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const unsigned char* p)
{
	uint64_t v;
	if (little_endian()) { memcpy(&v, p, 8); return v; }
	return rd32(p) | (uint64_t)rd32(p + 4) << 32;
}
static void wr32(unsigned char* p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static void wr64(unsigned char* p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

/*------   XXH32 (doc/frame_format.md, section 8.1), incremental   ------*/
#define P1 0x9E3779B1u
#define P2 0x85EBCA77u
#define P3 0xC2B2AE3Du
#define P4 0x27D4EB2Fu
#define P5 0x165667B1u
typedef struct { uint32_t v[4]; uint32_t seed; uint64_t total; unsigned char buf[16]; unsigned bufLen; } XXH32_state;

static uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
static uint32_t xxh_round(uint32_t v, uint32_t x) { return rotl32(v + x * P2, 13) * P1; }

static void xxh_reset(XXH32_state* s, uint32_t seed)
{
	s->v[0] = seed + P1 + P2; s->v[1] = seed + P2; s->v[2] = seed; s->v[3] = seed - P1;
	s->seed = seed; s->total = 0; s->bufLen = 0;
}

static void xxh_update(XXH32_state* s, const unsigned char* p, size_t n)
{
	s->total += n;
	if (s->bufLen) {                                    /* complete the buffered stripe */
		const size_t k = n < 16 - s->bufLen ? n : 16 - s->bufLen;
		memcpy(s->buf + s->bufLen, p, k);
		s->bufLen += (unsigned)k; p += k; n -= k;
		if (s->bufLen < 16) return;
		for (int i = 0; i < 4; i++) s->v[i] = xxh_round(s->v[i], rd32(s->buf + 4 * i));
		s->bufLen = 0;
	}
	for (; n >= 16; p += 16, n -= 16)
		for (int i = 0; i < 4; i++) s->v[i] = xxh_round(s->v[i], rd32(p + 4 * i));
	memcpy(s->buf, p, n);
	s->bufLen = (unsigned)n;
}

static uint32_t xxh_digest(const XXH32_state* s)
{
	uint32_t h = s->total >= 16 ? rotl32(s->v[0], 1) + rotl32(s->v[1], 7) + rotl32(s->v[2], 12) + rotl32(s->v[3], 18)
	                            : s->seed + P5;
	h += (uint32_t)s->total;
	unsigned i = 0;
	for (; i + 4 <= s->bufLen; i += 4) h = rotl32(h + rd32(s->buf + i) * P3, 17) * P4;
	for (; i < s->bufLen; i++) h = rotl32(h + s->buf[i] * P5, 11) * P1;
	h ^= h >> 15; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
	return h;
}

unsigned WZF_XXH32(const void* src, size_t srcSize, unsigned seed)
{
	XXH32_state s;
	xxh_reset(&s, seed);
	xxh_update(&s, (const unsigned char*)src, srcSize);
	return xxh_digest(&s);
}

/*------   XXH64 (doc/frame_format.md, section 8.2), incremental: a stripe of 32 bytes is four 64-bit lanes,
   twice as much per step as XXH32 on 64-bit processors   ------*/
#define Q1 0x9E3779B185EBCA87ull
#define Q2 0xC2B2AE3D27D4EB4Full
#define Q3 0x165667B19E3779F9ull
#define Q4 0x85EBCA77C2B2AE63ull
#define Q5 0x27D4EB2F165667C5ull
typedef struct { uint64_t v[4]; uint64_t seed, total; unsigned char buf[32]; unsigned bufLen; } XXH64_state;

static uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t xxh64_round(uint64_t v, uint64_t x) { return rotl64(v + x * Q2, 31) * Q1; }
static uint64_t xxh64_merge(uint64_t h, uint64_t v) { return (h ^ xxh64_round(0, v)) * Q1 + Q4; }

static void xxh64_reset(XXH64_state* s, uint64_t seed)
{
	s->v[0] = seed + Q1 + Q2; s->v[1] = seed + Q2; s->v[2] = seed; s->v[3] = seed - Q1;
	s->seed = seed; s->total = 0; s->bufLen = 0;
}

static void xxh64_stripes(uint64_t v[4], const unsigned char* p, size_t n)    /* n a multiple of 32 */
{
	uint64_t a = v[0], b = v[1], c = v[2], d = v[3];
	for (; n; p += 32, n -= 32) {
		a = xxh64_round(a, rd64(p)); b = xxh64_round(b, rd64(p + 8));
		c = xxh64_round(c, rd64(p + 16)); d = xxh64_round(d, rd64(p + 24));
	}
	v[0] = a; v[1] = b; v[2] = c; v[3] = d;
}

static void xxh64_update(XXH64_state* s, const unsigned char* p, size_t n)
{
	s->total += n;
	if (s->bufLen) {                                    /* complete the buffered stripe */
		const size_t k = n < 32 - s->bufLen ? n : 32 - s->bufLen;
		memcpy(s->buf + s->bufLen, p, k);
		s->bufLen += (unsigned)k; p += k; n -= k;
		if (s->bufLen < 32) return;
		xxh64_stripes(s->v, s->buf, 32);
		s->bufLen = 0;
	}
	xxh64_stripes(s->v, p, n & ~(size_t)31);
	p += n & ~(size_t)31;
	n &= 31;
	memcpy(s->buf, p, n);
	s->bufLen = (unsigned)n;
}

static uint64_t xxh64_digest(const XXH64_state* s)
{
	uint64_t h;
	if (s->total >= 32) {
		h = rotl64(s->v[0], 1) + rotl64(s->v[1], 7) + rotl64(s->v[2], 12) + rotl64(s->v[3], 18);
		for (int i = 0; i < 4; i++) h = xxh64_merge(h, s->v[i]);
	}
	else h = s->seed + Q5;
	h += s->total;
	unsigned i = 0;
	for (; i + 8 <= s->bufLen; i += 8) h = rotl64(h ^ xxh64_round(0, rd64(s->buf + i)), 27) * Q1 + Q4;
	if (i + 4 <= s->bufLen) { h = rotl64(h ^ (uint64_t)rd32(s->buf + i) * Q1, 23) * Q2 + Q3; i += 4; }
	for (; i < s->bufLen; i++) h = rotl64(h ^ s->buf[i] * Q5, 11) * Q1;
	h ^= h >> 33; h *= Q2; h ^= h >> 29; h *= Q3; h ^= h >> 32;
	return h;
}

unsigned long long WZF_XXH64(const void* src, size_t srcSize, unsigned long long seed)
{
	XXH64_state s;
	xxh64_reset(&s, seed);
	xxh64_update(&s, (const unsigned char*)src, srcSize);
	return xxh64_digest(&s);
}

/* The content checksum of a frame: XXH32, or XXH64's low 32 bits */
typedef struct { int xxh64; XXH32_state h32; XXH64_state h64; } Checksum;
static void ck_reset(Checksum* c, int xxh64) { c->xxh64 = xxh64; if (xxh64) xxh64_reset(&c->h64, 0); else xxh_reset(&c->h32, 0); }
static void ck_update(Checksum* c, const unsigned char* p, size_t n) { if (c->xxh64) xxh64_update(&c->h64, p, n); else xxh_update(&c->h32, p, n); }
static uint32_t ck_digest(const Checksum* c) { return c->xxh64 ? (uint32_t)xxh64_digest(&c->h64) : xxh_digest(&c->h32); }

/*------   Linked blocks: the window   ------
   A buffer holds the last *len bytes of a frame's content; the next n bytes refer to the last min(*len, D) of them.
   win_room makes room for those n bytes right after them, by moving them to the front or by allocating a larger
   buffer (with room for D more bytes, up to limit, the content size), and returns where the n bytes go. */
static unsigned char* win_room(unsigned char** buf, size_t* cap, size_t* len, size_t D, size_t n, unsigned long long limit)
{
	const size_t keep = *len < D ? *len : D;
	if (*len + n <= *cap) return *buf + *len;
	if (keep + n <= *cap) {
		memmove(*buf, *buf + *len - keep, keep);
		*len = keep;
		return *buf + keep;
	}
	size_t want = keep + n + D;
	if (want > limit) want = (size_t)limit;
	if (want < keep + n) want = keep + n;
	unsigned char* const q = (unsigned char*)malloc(want);
	if (!q) return NULL;
	if (keep) memcpy(q, *buf + *len - keep, keep);
	free(*buf);
	*buf = q; *cap = want; *len = keep;
	return q + keep;
}

/*------   Compression   ------*/
struct WZF_CCtx_s {
	WZF_params p;
	int blockLog, checksum, stage;      /* stage: 0 before Begin, 1 in blocks, 2 ended */
	unsigned long long contentSize, consumed;
	Checksum xxh;
	WLZ_State_Str* ws;
	WLZhc_State_Str* hs;
	unsigned char* scratch;
	size_t scratchCap;
	unsigned char* win;                 /* linked blocks: the window, then the input being compressed */
	size_t winCap, winLen;
};

/* By default (windowLog 0, blockLog 0) WZIP content is one block, up to 1 GiB, unless there are more threads than one
   block can use (levels 0-6: 1; 7-13: WZIP_WORKERS_MAX): then content over one block of WZF_LINKED_BLOCKLOG, or of
   unknown size, is cut into such blocks, compressed at once and linked (independent at level 0, whose window is 1 MiB) */
static int split_by_threads(const WZF_params* p, unsigned long long contentSize)
{
	const int oneBlock = p->level >= 7 ? WZIP_WORKERS_MAX : 1;
	return p->codec == WZF_CODEC_WZIP && p->windowLog == 0 && p->blockLog == 0 && p->nbWorkers > oneBlock
	       && (contentSize == WZF_CONTENTSIZE_UNKNOWN || contentSize > 1ULL << WZF_LINKED_BLOCKLOG);
}

/* the window log of the frame's linked blocks, 0 for independent blocks (WZF_params.windowLog) */
static int frame_windowLog(const WZF_params* p, unsigned long long contentSize)
{
	if (p->codec != WZF_CODEC_WZIP || p->windowLog < 0) return 0;
	if (p->windowLog) return p->windowLog;
	return split_by_threads(p, contentSize) && p->level > 0 ? WZIP_LEVEL_WINDOW_LOG(p->level) : 0;   /* the level's window */
}

/* blockLog 0: one block for the content, up to 1 GiB, or for content of unknown size the codec's widest window; blocks
   of at most WZF_LINKED_BLOCKLOG when split by the threads or linked */
static int auto_blockLog(const WZF_params* p, unsigned long long contentSize)
{
	const int linked = p->codec == WZF_CODEC_WZIP && (p->windowLog > 0 || split_by_threads(p, contentSize));
	if (contentSize == WZF_CONTENTSIZE_UNKNOWN) return linked ? WZF_LINKED_BLOCKLOG : p->codec == WZF_CODEC_WZIP ? 27 : 23;
	const int top = linked ? WZF_LINKED_BLOCKLOG : 30;
	int b = 16;
	while (b < top && (1ULL << b) < contentSize) b++;
	return b;
}

static size_t codec_max(int codec) { return codec == WZF_CODEC_WZIP ? WZIP_MAX_INPUT_SIZE : WLZ_MAX_INPUT_SIZE; }

static int params_ok(const WZF_params* p)
{
	if (p->codec == WZF_CODEC_WZIP) { if (p->level < 0 || p->level > 13) return 0; }
	else if (p->codec == WZF_CODEC_WLZ4) { if (p->level < -2 || p->level > 12) return 0; }
	else return 0;
	if (p->windowLog < -1 || (p->windowLog > 0 && (p->codec != WZF_CODEC_WZIP || p->windowLog < WZF_WINDOWLOG_MIN
	                                               || p->windowLog > WZF_WINDOWLOG_MAX || p->blockLog > 30)))
		return 0;
	if (p->checksumType != WZF_CHECKSUM_XXH64 && p->checksumType != WZF_CHECKSUM_XXH32) return 0;
	return p->blockLog == 0 || (p->blockLog >= WZF_BLOCKLOG_MIN && p->blockLog <= WZF_BLOCKLOG_MAX);
}

WZF_CCtx* WZF_createCCtx(void) { return (WZF_CCtx*)calloc(1, sizeof(WZF_CCtx)); }

void WZF_freeCCtx(WZF_CCtx* c)
{
	if (!c) return;
	if (c->ws) WLZ_Free_State(c->ws);
	if (c->hs) WLZhc_Free_State(c->hs);
	free(c->scratch);
	free(c->win);
	free(c);
}

size_t WZF_blockBound(size_t srcSize) { return WZF_BLOCK_HEADER + srcSize + WZIP_MEM_OVERHEAD; }

int WZF_blockLog(const WZF_CCtx* c) { return c->blockLog; }

size_t WZF_compressBegin(WZF_CCtx* c, void* dst, size_t dstCapacity, const WZF_params* params,
                         unsigned long long contentSize)
{
	if (!c || !dst || !params || !params_ok(params)) return ERR(parameter);
	const int windowLog = frame_windowLog(params, contentSize);
	const int hasSize = contentSize != WZF_CONTENTSIZE_UNKNOWN, linked = windowLog != 0;
	const size_t hdr = WZF_HEADER_MIN + linked + (hasSize ? 8 : 0);
	if (dstCapacity < hdr) return ERR(dstSize_tooSmall);
	c->p = *params;
	c->p.windowLog = windowLog;                         /* from here on, 0: independent blocks */
	c->blockLog = params->blockLog ? params->blockLog : auto_blockLog(params, contentSize);
	c->checksum = !params->noChecksum;
	c->contentSize = contentSize;
	c->consumed = 0;
	c->winLen = 0;
	ck_reset(&c->xxh, params->checksumType == WZF_CHECKSUM_XXH64);
	if (params->codec == WZF_CODEC_WLZ4) {
		if (params->level < 0 && !c->ws && !(c->ws = WLZ_New_State())) return ERR(memory);
		if (params->level >= 0 && !c->hs && !(c->hs = WLZhc_New_State())) return ERR(memory);
	}
	unsigned char* o = (unsigned char*)dst;
	wr32(o, WZF_MAGIC);
	o[4] = (unsigned char)(params->codec | (c->checksum ? WZF_FLAG_CHECKSUM : 0) | (hasSize ? WZF_FLAG_SIZE : 0)
	                       | (linked ? WZF_FLAG_LINKED : 0)
	                       | (c->checksum && params->checksumType == WZF_CHECKSUM_XXH64 ? WZF_FLAG_XXH64 : 0));
	o[5] = (unsigned char)(WZF_FRAME_VERSION << 4 | (params->codec == WZF_CODEC_WZIP && params->level >= 2 ? WZF_CODEC_VERSION_2 : WZF_CODEC_VERSION));
	o[6] = (unsigned char)c->blockLog;
	if (linked) o[7] = (unsigned char)windowLog;
	if (hasSize) wr64(o + 7 + linked, contentSize);
	c->stage = 1;
	return hdr;
}

/* compresses n bytes with the frame's codec into out (capacity cap), with the dictSize bytes before src as the
   dictionary (linked blocks) and up to `workers` threads (WZIP); returns the stream's size, 0 if it failed */
static size_t codec_compress(const WZF_params* p, WLZ_State_Str* ws, WLZhc_State_Str* hs, unsigned char* out,
                             size_t cap, const unsigned char* src, size_t n, size_t dictSize, int workers)
{
	if (p->codec == WZF_CODEC_WZIP) {
		int icap = cap > 0x7FFFFFFF ? 0x7FFFFFFF : (int)cap;
		const int r = wzip_compress_usingDict(src, (int)n, out, &icap, p->level, workers,
		                                      dictSize ? src - dictSize : NULL, (int)dictSize);
		return r > 0 ? (size_t)r : 0;
	}
	const unsigned ucap = cap > 0xFFFFFFFFu ? 0xFFFFFFFFu : (unsigned)cap;
	if (p->level == -2) return WLZ_Compress_Fast(ws, (const char*)src, (char*)out, (unsigned)n, ucap, 1);
	if (p->level == -1) return WLZ_Compress(ws, (const char*)src, (char*)out, (unsigned)n, ucap);
	return WLZhc_Compress(hs, (const char*)src, (char*)out, (unsigned)n, ucap, p->level);
}

static size_t codec_bound(int codec, size_t n)
{
	return codec == WZF_CODEC_WZIP ? n + WZIP_MEM_OVERHEAD : WLZ_COMPRESSBOUND(n);
}

static size_t max_block(const WZF_CCtx* c)
{
	const size_t b = (size_t)1 << c->blockLog;
	return b < codec_max(c->p.codec) ? b : codec_max(c->p.codec);
}

/* writes the block of n bytes at s into o (room for WZF_BLOCK_HEADER + n bytes), given its codec stream of cs bytes
   (0: none), raw if the stream does not shrink it; returns the bytes written */
static size_t put_block(unsigned char* o, const unsigned char* s, size_t n, const unsigned char* stream, size_t cs)
{
	if (cs == 0 || cs >= n) {
		memcpy(o + WZF_BLOCK_HEADER, s, n);
		wr32(o, (uint32_t)n | 0x80000000u);
		return WZF_BLOCK_HEADER + n;
	}
	if (stream != o + WZF_BLOCK_HEADER) memcpy(o + WZF_BLOCK_HEADER, stream, cs);
	wr32(o, (uint32_t)cs);
	return WZF_BLOCK_HEADER + cs;
}

/* compresses one block in this thread, in place in o if it has room for the codec's worst case */
static size_t compress_one(WZF_CCtx* c, unsigned char* o, size_t cap, const unsigned char* s, size_t n,
                           size_t dictSize, int workers)
{
	const size_t bound = codec_bound(c->p.codec, n);
	unsigned char* stream = o + WZF_BLOCK_HEADER;
	if (cap - WZF_BLOCK_HEADER < bound) {
		if (c->scratchCap < bound) {
			free(c->scratch);
			c->scratchCap = 0;
			if (!(c->scratch = (unsigned char*)malloc(bound))) return ERR(memory);
			c->scratchCap = bound;
		}
		stream = c->scratch;
	}
	return put_block(o, s, n, stream, codec_compress(&c->p, c->ws, c->hs, stream, bound, s, n, dictSize, workers));
}

#if WZIP_MULTITHREAD
/* Blocks compressed at once: each thread takes the next block until none is left, and keeps its stream */
typedef struct {
	const WZF_params* p;
	const unsigned char* buf;           /* the blocks: n bytes at buf + h, cut every maxBlk */
	size_t h, n, maxBlk, D;             /* D: the window of linked blocks (0: independent) */
	long nbBlocks, next, failed;
	int workers;                        /* threads of each WZIP block (its match finder's indexes) */
	unsigned char** out;                /* each block's stream (NULL: none), and its size */
	size_t* outSize;
} WZF_Jobs;

static void jobs_run(WZF_Jobs* j, WLZ_State_Str* ws, WLZhc_State_Str* hs)
{
	for (;;) {
		const long i = WZ_FETCH_ADD(&j->next, 1);
		if (i >= j->nbBlocks) return;
		const size_t at = j->h + (size_t)i * j->maxBlk, n = j->h + j->n - at < j->maxBlk ? j->h + j->n - at : j->maxBlk;
		const size_t bound = codec_bound(j->p->codec, n);
		unsigned char* const q = (unsigned char*)malloc(bound);
		if (!q) { WZ_STORE(&j->failed, 1); continue; }
		const size_t cs = codec_compress(j->p, ws, hs, q, bound, j->buf + at, n, at < j->D ? at : j->D, j->workers);
		if (cs == 0 || cs >= n) { free(q); continue; }
		unsigned char* const r = (unsigned char*)realloc(q, cs);
		j->out[i] = r ? r : q;
		j->outSize[i] = cs;
	}
}

WZ_THREAD_FN(jobs_main, arg)
{
	WZF_Jobs* const j = (WZF_Jobs*)arg;
	WLZ_State_Str* ws = NULL;
	WLZhc_State_Str* hs = NULL;
	if (j->p->codec == WZF_CODEC_WLZ4 && !(j->p->level < 0 ? (void*)(ws = WLZ_New_State()) : (void*)(hs = WLZhc_New_State())))
		return 0;                                       /* the other threads take the blocks */
	jobs_run(j, ws, hs);
	if (ws) WLZ_Free_State(ws);
	if (hs) WLZhc_Free_State(hs);
	return 0;
}
#endif

/* compresses the n bytes at buf + h as blocks into o; for linked blocks the bytes before them in buf are the frame's
   content before them, up to the window at least. Up to nbWorkers blocks at a time, each with nbWorkers / blocks
   threads of its own (at most WZIP_WORKERS_MAX, what its match finder uses); the same blocks in any case. */
static size_t compress_run(WZF_CCtx* c, unsigned char* o, size_t cap, const unsigned char* buf, size_t h, size_t n)
{
	const size_t maxBlk = max_block(c);
	const size_t D = c->p.windowLog ? (size_t)1 << c->p.windowLog : 0;
	const int threads = c->p.nbWorkers > 1 ? c->p.nbWorkers : 1;
	size_t pos = 0;
#if WZIP_MULTITHREAD
	const size_t blocks = (n - 1) / maxBlk + 1, jobs = blocks < (size_t)threads ? blocks : (size_t)threads;
	if (jobs > 1) {
		WZF_Jobs j;
		memset(&j, 0, sizeof j);
		j.p = &c->p; j.buf = buf; j.h = h; j.n = n; j.maxBlk = maxBlk; j.D = D;
		j.nbBlocks = (long)blocks;
		j.workers = threads / (int)jobs < WZIP_WORKERS_MAX ? threads / (int)jobs : WZIP_WORKERS_MAX;
		j.out = (unsigned char**)calloc(blocks, sizeof *j.out);
		j.outSize = (size_t*)calloc(blocks, sizeof *j.outSize);
		WZ_Thread* const t = (WZ_Thread*)malloc((jobs - 1) * sizeof *t);
		size_t started = 0;
		if (j.out && j.outSize && t) {
			while (started < jobs - 1 && WZ_THREAD_START(&t[started], jobs_main, &j)) started++;
			jobs_run(&j, c->ws, c->hs);                 /* this thread takes blocks too */
			for (size_t i = 0; i < started; i++) WZ_THREAD_JOIN(t[i]);
			if (j.failed) pos = ERR(memory);
			else for (size_t i = 0, at = h; i < blocks; i++, at += maxBlk) {
				const size_t k = h + n - at < maxBlk ? h + n - at : maxBlk;
				if (cap - pos < WZF_BLOCK_HEADER + k) { pos = ERR(dstSize_tooSmall); break; }   /* as one by one */
				pos += put_block(o + pos, buf + at, k, j.out[i], j.out[i] ? j.outSize[i] : 0);
			}
		}
		else pos = ERR(memory);
		if (j.out) for (size_t i = 0; i < blocks; i++) free(j.out[i]);
		free(j.out); free(j.outSize); free(t);
		return pos;
	}
#endif
	for (size_t at = h; at < h + n; ) {
		const size_t k = h + n - at < maxBlk ? h + n - at : maxBlk;
		if (cap - pos < WZF_BLOCK_HEADER + k) return ERR(dstSize_tooSmall);
		const size_t r = compress_one(c, o + pos, cap - pos, buf + at, k, at < D ? at : D, threads);
		if (WZF_isError(r)) return r;
		pos += r; at += k;
	}
	return pos;
}

/* compresses n bytes of content at s as blocks; linked blocks refer to the content before s: the window, or, if
   inPlace (the one-shot compressor), the bytes before s */
static size_t compress_src(WZF_CCtx* c, unsigned char* o, size_t cap, const unsigned char* s, size_t n, int inPlace)
{
	if (c->contentSize != WZF_CONTENTSIZE_UNKNOWN && n > c->contentSize - c->consumed) return ERR(contentSize);
	if (cap < WZF_BLOCK_HEADER + (n < max_block(c) ? n : max_block(c))) return ERR(dstSize_tooSmall);   /* the first */
	size_t r;
	if (c->p.windowLog && !inPlace) {
		unsigned char* const at = win_room(&c->win, &c->winCap, &c->winLen, (size_t)1 << c->p.windowLog, n, c->contentSize);
		if (!at) return ERR(memory);
		memcpy(at, s, n);
		r = compress_run(c, o, cap, c->win, (size_t)(at - c->win), n);
		if (!WZF_isError(r)) c->winLen = (size_t)(at - c->win) + n;
	}
	else r = compress_run(c, o, cap, s, 0, n);
	if (WZF_isError(r)) return r;
	if (c->checksum) ck_update(&c->xxh, s, n);
	c->consumed += n;
	return r;
}

size_t WZF_compressBlock(WZF_CCtx* c, void* dst, size_t dstCapacity, const void* src, size_t srcSize)
{
	if (!c || c->stage != 1 || (!src && srcSize) || (!dst && dstCapacity)) return ERR(parameter);
	if (srcSize == 0) return 0;
	if (srcSize > max_block(c)) return ERR(parameter);
	return compress_src(c, (unsigned char*)dst, dstCapacity, (const unsigned char*)src, srcSize, 0);
}

size_t WZF_compressBlocks(WZF_CCtx* c, void* dst, size_t dstCapacity, const void* src, size_t srcSize)
{
	if (!c || c->stage != 1 || (!src && srcSize) || (!dst && dstCapacity)) return ERR(parameter);
	if (srcSize == 0) return 0;
	return compress_src(c, (unsigned char*)dst, dstCapacity, (const unsigned char*)src, srcSize, 0);
}

size_t WZF_compressEnd(WZF_CCtx* c, void* dst, size_t dstCapacity)
{
	if (!c || c->stage != 1) return ERR(parameter);
	if (c->contentSize != WZF_CONTENTSIZE_UNKNOWN && c->consumed != c->contentSize) return ERR(contentSize);
	const size_t n = WZF_BLOCK_HEADER + (c->checksum ? 4 : 0);
	if (dstCapacity < n) return ERR(dstSize_tooSmall);
	wr32((unsigned char*)dst, 0);
	if (c->checksum) wr32((unsigned char*)dst + 4, ck_digest(&c->xxh));
	c->stage = 2;
	return n;
}

size_t WZF_compressBound(size_t srcSize, const WZF_params* params)
{
	static const WZF_params byDefault = { WZF_CODEC_WZIP, 1, 0, 0, 256, 0, 0 };   /* the most blocks by default */
	if (!params) params = &byDefault;
	const int codec = params->codec;
	const int b = params->blockLog ? params->blockLog : auto_blockLog(params, srcSize);
	const size_t blk = (size_t)1 << b, maxBlk = blk < codec_max(codec) ? blk : codec_max(codec);
	const size_t blocks = srcSize ? (srcSize - 1) / maxBlk + 1 : 0;
	return WZF_HEADER_MAX + blocks * WZF_BLOCK_HEADER + srcSize + WZF_BLOCK_HEADER + 4;
}

size_t WZF_compress(void* dst, size_t dstCapacity, const void* src, size_t srcSize, const WZF_params* params)
{
	if (!dst || (!src && srcSize) || !params) return ERR(parameter);
	WZF_CCtx* c = WZF_createCCtx();
	if (!c) return ERR(memory);
	unsigned char* const o = (unsigned char*)dst;
	size_t pos = WZF_compressBegin(c, o, dstCapacity, params, srcSize);
	if (!WZF_isError(pos) && srcSize) {                 /* all blocks at once; linked ones refer to src in place */
		const size_t r = compress_src(c, o + pos, dstCapacity - pos, (const unsigned char*)src, srcSize, 1);
		pos = WZF_isError(r) ? r : pos + r;
	}
	if (!WZF_isError(pos)) {
		const size_t r = WZF_compressEnd(c, o + pos, dstCapacity - pos);
		pos = WZF_isError(r) ? r : pos + r;
	}
	WZF_freeCCtx(c);
	return pos;
}

/*------   Decompression   ------*/
struct WZF_DCtx_s {
	WZF_FrameHeader h;
	int stage;                          /* 0 header expected, 1 block header expected, 2 block data, 3 end, 4 done */
	int pendingRaw;
	size_t pendingSize;
	unsigned long long decoded;
	Checksum xxh;
	unsigned char* win;                 /* linked blocks: the window, then the block being decoded */
	size_t winCap, winLen;
};

WZF_DCtx* WZF_createDCtx(void) { return (WZF_DCtx*)calloc(1, sizeof(WZF_DCtx)); }
void WZF_freeDCtx(WZF_DCtx* d) { if (d) free(d->win); free(d); }
const WZF_FrameHeader* WZF_frameHeader(const WZF_DCtx* d) { return &d->h; }
size_t WZF_frameBlockSize(const WZF_DCtx* d) { return (size_t)1 << d->h.blockLog; }

size_t WZF_decompressBegin(WZF_DCtx* d, const void* src, size_t srcSize)
{
	const unsigned char* const s = (const unsigned char*)src;
	if (!d || !src) return ERR(parameter);
	if (srcSize < 4) return ERR(srcSize_wrong);
	const uint32_t magic = rd32(s);
	if ((magic & 0xFFFFFFF0u) == WZF_SKIPPABLE_MIN) {
		if (srcSize < 8) return 8;
		memset(&d->h, 0, sizeof d->h);
		d->h.skippable = 1;
		d->stage = 4;
		const uint64_t total = 8 + (uint64_t)rd32(s + 4);
		return total > (size_t)-1 - WZF_error_maxCode ? ERR(unsupported) : (size_t)total;
	}
	if (magic != WZF_MAGIC) return ERR(unknown_frame);
	if (srcSize < WZF_HEADER_MIN) return ERR(srcSize_wrong);
	const unsigned flg = s[4], ver = s[5], bs = s[6];
	const int linked = (flg & WZF_FLAG_LINKED) != 0;
	/* FLG bits 5-6: the checksum type, 0 (XXH32) or 1 (XXH64), 0 without a checksum; bit 7 reserved */
	if ((flg & 3) > WZF_CODEC_WLZ4 || (flg & 0xC0) || ((flg & WZF_FLAG_XXH64) && !(flg & WZF_FLAG_CHECKSUM))
	    || (ver >> 4) != WZF_FRAME_VERSION
	    || ((ver & 15) != WZF_CODEC_VERSION && ((ver & 15) != WZF_CODEC_VERSION_2 || (flg & 3) != WZF_CODEC_WZIP))
	    || bs < WZF_BLOCKLOG_MIN || bs > WZF_BLOCKLOG_MAX || (linked && ((flg & 3) != WZF_CODEC_WZIP || bs > 30)))
		return ERR(unsupported);
	const size_t hdr = WZF_HEADER_MIN + linked + ((flg & WZF_FLAG_SIZE) ? 8 : 0);
	if (srcSize < hdr) return hdr;
	if (linked && (s[7] < WZF_WINDOWLOG_MIN || s[7] > WZF_WINDOWLOG_MAX)) return ERR(unsupported);
	d->h.codec = (int)(flg & 3);
	d->h.codecVersion = (int)(ver & 15);
	d->h.blockLog = (int)bs;
	d->h.checksum = (flg & WZF_FLAG_CHECKSUM) != 0;
	d->h.checksumType = (flg & WZF_FLAG_XXH64) ? WZF_CHECKSUM_XXH64 : WZF_CHECKSUM_XXH32;
	d->h.skippable = 0;
	d->h.windowLog = linked ? s[7] : 0;
	d->h.contentSize = (flg & WZF_FLAG_SIZE) ? rd64(s + 7 + linked) : WZF_CONTENTSIZE_UNKNOWN;
	if (d->h.contentSize == WZF_CONTENTSIZE_UNKNOWN && (flg & WZF_FLAG_SIZE)) return ERR(corrupted);
	d->stage = 1;
	d->decoded = 0;
	d->winLen = 0;
	ck_reset(&d->xxh, (flg & WZF_FLAG_XXH64) != 0);
	return hdr;
}

size_t WZF_nextBlock(WZF_DCtx* d, const void* blockHeader, int* isRaw)
{
	if (!d || d->stage != 1 || !blockHeader) return ERR(parameter);
	const uint32_t v = rd32((const unsigned char*)blockHeader);
	if (isRaw) *isRaw = 0;
	if (v == 0) { d->stage = 3; return 0; }
	const size_t c = v & 0x7FFFFFFFu;
	const int raw = (int)(v >> 31);
	if (c == 0 || (raw && c > ((size_t)1 << d->h.blockLog))) return ERR(corrupted);
	d->pendingRaw = raw;
	d->pendingSize = c;
	d->stage = 2;
	if (isRaw) *isRaw = raw;
	return c;
}

/* decodes one compressed block of n bytes into dst, with the dictSize bytes before dst as the dictionary (linked
   blocks); returns its decoded size or an error */
static size_t decode_block(WZF_DCtx* d, unsigned char* dst, size_t cap, const unsigned char* s, size_t n, size_t dictSize)
{
	const size_t maxBlk = (size_t)1 << d->h.blockLog;
	if (d->h.codec == WZF_CODEC_WZIP) {
		if (n < 2 || n > 0x7FFFFFFF) return ERR(corrupted);
		int left = (int)n;
		const int ds = WZIP_Read_DecSize(s, &left);
		const size_t size = ds ? (size_t)ds : (size_t)left;     /* 0: stored, the rest of the block */
		if (size == 0 || size > maxBlk) return ERR(corrupted);
		if (cap < size) return ERR(dstSize_tooSmall);
		int dcap = (int)size;
		if (wzip_decompress_usingDict(s, (int)n, dst, &dcap, dictSize ? dst - dictSize : NULL, (int)dictSize) != (int)size)
			return ERR(corrupted);
		return size;
	}
	if (n > 0xFFFFFFFFu) return ERR(corrupted);
	const size_t size = WLZ_Read_DecSize((const char*)s, (unsigned)n);
	if (size == 0 || size > maxBlk) return ERR(corrupted);
	if (cap < size) return ERR(dstSize_tooSmall);
	return WLZ_Decompress((const char*)s, (char*)dst, (unsigned)n, (unsigned)size) == size ? size : ERR(corrupted);
}

size_t WZF_blockDecodedSize(const WZF_DCtx* d, const void* src, size_t srcSize)
{
	const unsigned char* const s = (const unsigned char*)src;
	const size_t c = d ? d->pendingSize : 0, maxBlk = d ? (size_t)1 << d->h.blockLog : 0;
	if (!d || d->stage != 2 || !src || srcSize < (c < 4 ? c : 4)) return ERR(parameter);
	if (d->pendingRaw) return c;
	size_t size;
	if (d->h.codec == WZF_CODEC_WZIP) {                 /* WZIP_Read_DecSize; 0: stored, the rest of the block */
		if (c < 2) return ERR(corrupted);
		size = (size_t)s[0] | (size_t)s[1] << 8;
		if (size >> 15) {
			if (c < 4) return ERR(corrupted);
			size = (size & 0x7FFF) | ((size_t)s[2] | (size_t)s[3] << 8) << 15;
		}
		else if (size == 0) size = c - 2;
	}
	else {                                              /* WLZ4: the same two forms, no stored form */
		if (c < 2) return ERR(corrupted);
		size = (size_t)s[0] | (size_t)s[1] << 8;
		if (size >> 15) {
			if (c < 4) return ERR(corrupted);
			size = (size & 0x7FFF) | ((size_t)s[2] | (size_t)s[3] << 8) << 15;
		}
	}
	return size == 0 || size > maxBlk ? ERR(corrupted) : size;
}

/* decodes the pending block into dst; linked blocks refer to the content before it: the window, or, if inPlace (the
   one-shot decoder), the bytes before dst */
static size_t decompress_block(WZF_DCtx* d, unsigned char* dst, size_t dstCapacity, const unsigned char* src,
                               size_t srcSize, int inPlace)
{
	if (!d || d->stage != 2 || !src || (!dst && dstCapacity) || srcSize != d->pendingSize) return ERR(parameter);
	const size_t D = d->h.windowLog ? (size_t)1 << d->h.windowLog : 0;
	const size_t dictSize = d->decoded < D ? (size_t)d->decoded : D;
	size_t r;
	if (D && !inPlace) {                                /* decoded in the window, after the content it refers to */
		const size_t size = WZF_blockDecodedSize(d, src, srcSize);
		if (WZF_isError(size)) return size;
		if (dstCapacity < size) return ERR(dstSize_tooSmall);
		unsigned char* const at = win_room(&d->win, &d->winCap, &d->winLen, D, size, d->h.contentSize);
		if (!at) return ERR(memory);
		if (d->pendingRaw) { memcpy(at, src, srcSize); r = srcSize; }
		else r = decode_block(d, at, size, src, srcSize, dictSize);
		if (WZF_isError(r)) return r;
		memcpy(dst, at, r);
		d->winLen = (size_t)(at - d->win) + r;
	}
	else if (d->pendingRaw) {
		if (dstCapacity < srcSize) return ERR(dstSize_tooSmall);
		memcpy(dst, src, srcSize);
		r = srcSize;
	}
	else r = decode_block(d, dst, dstCapacity, src, srcSize, dictSize);
	if (WZF_isError(r)) return r;
	d->decoded += r;
	if (d->h.contentSize != WZF_CONTENTSIZE_UNKNOWN && d->decoded > d->h.contentSize) return ERR(contentSize);
	if (d->h.checksum) ck_update(&d->xxh, (const unsigned char*)dst, r);
	d->stage = 1;
	return r;
}

size_t WZF_decompressBlock(WZF_DCtx* d, void* dst, size_t dstCapacity, const void* src, size_t srcSize)
{
	return decompress_block(d, (unsigned char*)dst, dstCapacity, (const unsigned char*)src, srcSize, 0);
}

size_t WZF_endSize(const WZF_DCtx* d) { return d->stage == 3 && d->h.checksum ? 4 : 0; }

size_t WZF_decompressEnd(WZF_DCtx* d, const void* src, size_t srcSize)
{
	if (!d || d->stage != 3 || srcSize != WZF_endSize(d) || (srcSize && !src)) return ERR(parameter);
	if (d->h.checksum && rd32((const unsigned char*)src) != ck_digest(&d->xxh)) return ERR(checksum);
	if (d->h.contentSize != WZF_CONTENTSIZE_UNKNOWN && d->decoded != d->h.contentSize) return ERR(contentSize);
	d->stage = 4;
	return 0;
}

size_t WZF_decompress(void* dst, size_t dstCapacity, const void* src, size_t srcSize)
{
	if (!src || (!dst && dstCapacity)) return ERR(parameter);
	if (srcSize == 0) return ERR(srcSize_wrong);
	WZF_DCtx* d = WZF_createDCtx();
	if (!d) return ERR(memory);
	const unsigned char* s = (const unsigned char*)src;
	unsigned char* o = (unsigned char*)dst;
	size_t pos = 0, out = 0, r = 0;
	while (pos < srcSize) {
		r = WZF_decompressBegin(d, s + pos, srcSize - pos);
		if (WZF_isError(r)) break;
		if (r > srcSize - pos) { r = ERR(srcSize_wrong); break; }
		pos += r;
		if (d->h.skippable) continue;
		for (;;) {
			if (srcSize - pos < WZF_BLOCK_HEADER) { r = ERR(srcSize_wrong); break; }
			int raw;
			const size_t c = WZF_nextBlock(d, s + pos, &raw);
			if (WZF_isError(c)) { r = c; break; }
			pos += WZF_BLOCK_HEADER;
			if (c == 0) break;
			if (srcSize - pos < c) { r = ERR(srcSize_wrong); break; }
			r = decompress_block(d, o ? o + out : NULL, dstCapacity - out, s + pos, c, 1);   /* after the content */
			if (WZF_isError(r)) break;
			out += r; pos += c;
		}
		if (WZF_isError(r)) break;
		const size_t e = WZF_endSize(d);
		if (srcSize - pos < e) { r = ERR(srcSize_wrong); break; }
		r = WZF_decompressEnd(d, s + pos, e);
		if (WZF_isError(r)) break;
		pos += e;
	}
	WZF_freeDCtx(d);
	return WZF_isError(r) ? r : out;
}

unsigned long long WZF_getContentSize(const void* src, size_t srcSize)
{
	const unsigned char* s = (const unsigned char*)src;
	unsigned long long total = 0;
	size_t pos = 0;
	if (!src || srcSize == 0) return WZF_CONTENTSIZE_UNKNOWN;
	WZF_DCtx d;
	memset(&d, 0, sizeof d);
	while (pos < srcSize) {
		const size_t r = WZF_decompressBegin(&d, s + pos, srcSize - pos);
		if (WZF_isError(r) || r > srcSize - pos) return WZF_CONTENTSIZE_UNKNOWN;
		pos += r;
		if (d.h.skippable) continue;
		if (d.h.contentSize == WZF_CONTENTSIZE_UNKNOWN) return WZF_CONTENTSIZE_UNKNOWN;
		total += d.h.contentSize;
		for (;;) {                                      /* skip the blocks */
			if (srcSize - pos < WZF_BLOCK_HEADER) return WZF_CONTENTSIZE_UNKNOWN;
			const uint32_t v = rd32(s + pos);
			pos += WZF_BLOCK_HEADER;
			if (v == 0) break;
			if (srcSize - pos < (v & 0x7FFFFFFFu)) return WZF_CONTENTSIZE_UNKNOWN;
			pos += v & 0x7FFFFFFFu;
		}
		if (d.h.checksum) {
			if (srcSize - pos < 4) return WZF_CONTENTSIZE_UNKNOWN;
			pos += 4;
		}
	}
	return total;
}
