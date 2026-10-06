/*
 * wzip, wlz4: command-line compression with WZIP and WLZ4 in WZ frames (doc/frame_format.md)
 * Copyright (c) 2026-present, Yingquan (Cody) Wu.
 * SPDX-License-Identifier: BSD-2-Clause (see LICENSE)
 *
 * One program; called as wlz4 (or with --wlz4) it compresses with WLZ4, otherwise with WZIP. Decompression reads
 * either codec. Files are processed block by block, so their size is not limited by memory.
 */
#define _FILE_OFFSET_BITS 64
#ifdef _MSC_VER
#  define _CRT_SECURE_NO_WARNINGS                      /* strerror, strcpy, fopen: used safely here */
#  define _CRT_NONSTDC_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include "wzframe.h"
#include "WZIP.h"
#include "WLZ4.h"

#ifdef _WIN32
#  include <io.h>
#  include <fcntl.h>
#  include <sys/utime.h>
#  include <windows.h>
#  define isatty _isatty
#  define fileno _fileno
#  define unlink _unlink
typedef struct __stat64 stat_t;
#  define stat_fn _stat64
static void set_binary(FILE* f) { _setmode(_fileno(f), _O_BINARY); }
#else
#  include <unistd.h>
#  include <utime.h>
typedef struct stat stat_t;
#  define stat_fn stat
static void set_binary(FILE* f) { (void)f; }
#endif

enum { OP_COMPRESS, OP_DECOMPRESS, OP_TEST, OP_LIST, OP_BENCH };

static struct {
	int op, codec, level, levelSet, blockLog, noCheck, toStdout, force, rmSource, verbosity, benchEnd, threads;
	const char* outName;
	const char* prog;
} g = { OP_COMPRESS, WZF_CODEC_WZIP, 1, 0, 0, 0, 0, 0, 0, 1, -100, 1, NULL, "wzip" };

static const char* cur = "";                           /* the file being processed, for messages */

static const char* ext(int codec) { return codec == WZF_CODEC_WLZ4 ? ".wlz4" : ".wz"; }

static void msg(int level, const char* fmt, ...)
{
	if (g.verbosity < level) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static int fail(const char* what) { msg(1, "%s: %s: %s\n", g.prog, cur, what); return 0; }

static double now(void)
{
#ifdef _WIN32
	LARGE_INTEGER f, t;
	QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
	return (double)t.QuadPart / f.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static void usage(FILE* f)
{
	fprintf(f,
"Usage: %s [options] [files]          compress (or, with -d, decompress) files; none or -: standard input\n"
"\n"
"  -#         level: WZIP 0-13 (default 1; 7-13 optimal parsing, slow), WLZ4 0-12 (8-12 optimal parsing)\n"
"  --fast     WLZ4's fast mode          --lazy  WLZ4's lazy mode (wlz4's default)\n"
"  --wzip     compress with WZIP (.wz)  --wlz4  compress with WLZ4 (.wlz4)\n"
"  -d         decompress                -t      test compressed files      -l  list their frames\n"
"  -c         write to standard output  -o FILE write to FILE (one input)\n"
"  -f         overwrite, or write compressed data to a terminal\n"
"  -k         keep the input files (default)            --rm  remove them after success\n"
"  -B#        blocks of at most 2^# bytes (10-31; default: the whole file, up to 1 GiB)\n"
"  -T#        threads for WZIP's optimal levels 7-13 (2-4 help; default 1); the output is the same\n"
"  --no-check no content checksum\n"
"  -b         benchmark the level (in memory, no files written); -e# up to level #\n"
"  -q, -v     fewer, more messages       -h  this help    -V  version\n",
		g.prog);
}

/*------   Buffers and I/O   ------*/
typedef struct { unsigned char* p; size_t cap; } Buf;

static int buf_reserve(Buf* b, size_t n)
{
	if (b->cap >= n) return 1;
	unsigned char* q = (unsigned char*)realloc(b->p, n);
	if (!q) return 0;
	b->p = q; b->cap = n;
	return 1;
}

/* reads up to n bytes, looping on short reads (pipes); returns the count */
static size_t read_full(FILE* f, void* p, size_t n)
{
	size_t got = 0;
	while (got < n) {
		const size_t r = fread((char*)p + got, 1, n - got, f);
		if (r == 0) break;
		got += r;
	}
	return got;
}

static int write_all(FILE* f, const void* p, size_t n) { return n == 0 || fwrite(p, 1, n, f) == n; }

static unsigned rd32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static int has_suffix(const char* s, const char* suf)
{
	const size_t a = strlen(s), b = strlen(suf);
	return a > b && !strcmp(s + a - b, suf);
}

static int file_exists(const char* name) { stat_t st; return stat_fn(name, &st) == 0; }

/*------   Compression   ------*/
static int compress_stream(FILE* in, FILE* out, unsigned long long contentSize, unsigned long long* inTotal,
                           unsigned long long* outTotal)
{
	WZF_params p = { g.codec, g.level, g.blockLog, g.noCheck, g.threads };
	WZF_CCtx* c = WZF_createCCtx();
	Buf src = { 0 }, dst = { 0 };
	int ok = 0;
	*inTotal = *outTotal = 0;
	if (!c || !buf_reserve(&dst, WZF_HEADER_MAX)) { fail("out of memory"); goto done; }
	size_t r = WZF_compressBegin(c, dst.p, dst.cap, &p, contentSize);
	if (WZF_isError(r)) { fail(WZF_getErrorName(r)); goto done; }
	if (!write_all(out, dst.p, r)) goto write_error;
	*outTotal += r;
	const size_t codecMax = g.codec == WZF_CODEC_WZIP ? WZIP_MAX_INPUT_SIZE : WLZ_MAX_INPUT_SIZE;
	size_t blk = (size_t)1 << WZF_blockLog(c);
	if (blk > codecMax) blk = codecMax;
	if (contentSize != WZF_CONTENTSIZE_UNKNOWN && contentSize < blk) blk = contentSize ? (size_t)contentSize : 1;
	if (!buf_reserve(&src, blk) || !buf_reserve(&dst, WZF_blockBound(blk))) { fail("out of memory"); goto done; }
	for (;;) {
		const size_t n = read_full(in, src.p, blk);
		if (ferror(in)) { fail("read error"); goto done; }
		if (n == 0) break;
		r = WZF_compressBlock(c, dst.p, dst.cap, src.p, n);
		if (WZF_isError(r)) {
			fail(WZF_getErrorCode(r) == WZF_error_contentSize ? "the input changed while it was read" : WZF_getErrorName(r));
			goto done;
		}
		if (!write_all(out, dst.p, r)) goto write_error;
		*inTotal += n; *outTotal += r;
		msg(3, "\r%llu => %llu bytes", *inTotal, *outTotal);
		if (n < blk) break;
	}
	r = WZF_compressEnd(c, dst.p, dst.cap);
	if (WZF_isError(r)) {
		fail(WZF_getErrorCode(r) == WZF_error_contentSize ? "the input changed while it was read" : WZF_getErrorName(r));
		goto done;
	}
	if (!write_all(out, dst.p, r)) goto write_error;
	*outTotal += r;
	ok = 1;
	goto done;
write_error:
	fail(strerror(errno));
done:
	WZF_freeCCtx(c);
	free(src.p); free(dst.p);
	return ok;
}

/*------   Decompression (and testing, listing)   ------*/
typedef struct { unsigned long long frames, blocks; int codecs, checksum, blockLog; } ListInfo;

/* decodes every frame of in into out (NULL: only checks them, for -t and -l); info collects what -l prints */
static int decompress_stream(FILE* in, FILE* out, unsigned long long* inTotal, unsigned long long* outTotal, ListInfo* info)
{
	WZF_DCtx* d = WZF_createDCtx();
	Buf cmp = { 0 }, dec = { 0 };
	unsigned char hdr[WZF_HEADER_MAX];
	int ok = 0;
	*inTotal = *outTotal = 0;
	if (!d) { fail("out of memory"); goto done; }
	for (int first = 1; ; first = 0) {
		size_t n = read_full(in, hdr, 4);
		if (n == 0 && !first) { ok = 1; break; }       /* after the last frame */
		if (n < 4) { fail(first ? "not in WZ format" : "trailing garbage after the last frame"); goto done; }
		*inTotal += 4;
		const unsigned magic = rd32(hdr);
		if ((magic & 0xFFFFFFF0u) == WZF_SKIPPABLE_MIN) {   /* a skippable frame: skip its data */
			if (read_full(in, hdr + 4, 4) != 4) { fail("truncated skippable frame"); goto done; }
			unsigned long long left = rd32(hdr + 4);
			unsigned char tmp[4096];
			*inTotal += 4;
			while (left) {
				const size_t k = read_full(in, tmp, left < sizeof tmp ? (size_t)left : sizeof tmp);
				if (k == 0) { fail("truncated skippable frame"); goto done; }
				left -= k; *inTotal += k;
			}
			continue;
		}
		if (magic != WZF_MAGIC) { fail(first ? "not in WZ format" : "data after the last frame is not a WZ frame"); goto done; }
		n += read_full(in, hdr + 4, WZF_HEADER_MIN - 4);
		size_t h = WZF_decompressBegin(d, hdr, n);
		if (!WZF_isError(h) && h > n && h <= sizeof hdr) {
			n += read_full(in, hdr + n, h - n);
			h = WZF_decompressBegin(d, hdr, n);
		}
		if (WZF_isError(h)) { fail(WZF_getErrorName(h)); goto done; }
		if (h != n) { fail("truncated frame"); goto done; }
		*inTotal += n - 4;
		const WZF_FrameHeader* fh = WZF_frameHeader(d);
		if (info) {
			info->frames++;
			info->codecs |= 1 << fh->codec;
			info->checksum |= fh->checksum;
			if (fh->blockLog > info->blockLog) info->blockLog = fh->blockLog;
		}
		for (;;) {
			unsigned char bh[WZF_BLOCK_HEADER];
			if (read_full(in, bh, sizeof bh) != sizeof bh) { fail("truncated frame"); goto done; }
			*inTotal += sizeof bh;
			int raw;
			const size_t c = WZF_nextBlock(d, bh, &raw);
			if (WZF_isError(c)) { fail(WZF_getErrorName(c)); goto done; }
			if (c == 0) break;
			if (!buf_reserve(&cmp, c)) { fail("out of memory"); goto done; }
			if (read_full(in, cmp.p, c) != c) { fail("truncated frame"); goto done; }
			*inTotal += c;
			const size_t size = WZF_blockDecodedSize(d, cmp.p, c);
			if (WZF_isError(size)) { fail(WZF_getErrorName(size)); goto done; }
			if (!buf_reserve(&dec, size)) { fail("out of memory"); goto done; }
			const size_t r = WZF_decompressBlock(d, dec.p, dec.cap, cmp.p, c);
			if (WZF_isError(r)) { fail(WZF_getErrorName(r)); goto done; }
			if (out && !write_all(out, dec.p, r)) { fail(strerror(errno)); goto done; }
			*outTotal += r;
			if (info) info->blocks++;
			msg(3, "\r%llu => %llu bytes", *inTotal, *outTotal);
		}
		const size_t e = WZF_endSize(d);
		unsigned char tail[4];
		if (e && read_full(in, tail, e) != e) { fail("truncated frame"); goto done; }
		*inTotal += e;
		const size_t r = WZF_decompressEnd(d, tail, e);
		if (WZF_isError(r)) { fail(WZF_getErrorName(r)); goto done; }
	}
done:
	if (ferror(in)) ok = fail("read error");
	WZF_freeDCtx(d);
	free(cmp.p); free(dec.p);
	return ok;
}

/*------   Files   ------*/
static FILE* open_out(const char* name)
{
	if (!g.force && file_exists(name)) { msg(1, "%s: %s already exists; not overwritten (use -f)\n", g.prog, name); return NULL; }
	FILE* f = fopen(name, "wb");
	if (!f) msg(1, "%s: cannot write %s: %s\n", g.prog, name, strerror(errno));
	return f;
}

static void copy_time(const char* from, const char* to)
{
	stat_t st;
	if (stat_fn(from, &st) != 0) return;
#ifdef _WIN32
	struct __utimbuf64 t;
	t.actime = st.st_atime; t.modtime = st.st_mtime;
	_utime64(to, &t);
#else
	struct utimbuf t;
	t.actime = st.st_atime; t.modtime = st.st_mtime;
	utime(to, &t);
#endif
}

static int process(const char* inName)
{
	const int isStdin = !strcmp(inName, "-");
	char* outName = NULL;
	cur = isStdin ? "(stdin)" : inName;
	unsigned long long contentSize = WZF_CONTENTSIZE_UNKNOWN;
	if (!isStdin) {
		stat_t st;
		if (stat_fn(inName, &st) != 0) return fail(strerror(errno));
		if ((st.st_mode & S_IFMT) == S_IFDIR) return fail("a directory; skipped");
		if ((st.st_mode & S_IFMT) == S_IFREG) contentSize = (unsigned long long)st.st_size;
	}
	FILE* in = isStdin ? stdin : fopen(inName, "rb");
	if (!in) return fail(strerror(errno));
	if (isStdin) set_binary(stdin);
	int ok = 0;
	unsigned long long inTotal = 0, outTotal = 0;
	const double t0 = now();

	if (g.op == OP_LIST || g.op == OP_TEST) {
		ListInfo info = { 0 };
		ok = decompress_stream(in, NULL, &inTotal, &outTotal, g.op == OP_LIST ? &info : NULL);
		if (g.op == OP_LIST && ok)
			printf("%-30s %6llu %7llu  %-5s %3d  %-5s %14llu %14llu  %6.3f\n", cur, info.frames, info.blocks,
			       info.codecs == 1 ? "WZIP" : info.codecs == 2 ? "WLZ4" : info.codecs ? "mixed" : "-", info.blockLog,
			       info.checksum ? "XXH32" : "-", inTotal, outTotal, inTotal ? (double)outTotal / inTotal : 0.0);
		else if (ok) msg(2, "%s: OK (%llu bytes)\n", cur, outTotal);
		if (!isStdin) fclose(in);
		return ok;
	}

	/* the output: standard output, -o, or the input's name with the suffix added or removed */
	const int toStdout = g.toStdout || (g.outName && !strcmp(g.outName, "-")) || (isStdin && !g.outName);
	if (toStdout) outName = NULL;
	else if (g.outName) outName = strdup(g.outName);
	else if (g.op == OP_COMPRESS) {
		const char* e = ext(g.codec);
		if ((has_suffix(inName, ".wz") || has_suffix(inName, ".wlz4")) && !g.force) {
			if (!isStdin) fclose(in);
			return fail("already compressed; skipped (use -f)");
		}
		outName = (char*)malloc(strlen(inName) + strlen(e) + 1);
		if (outName) { strcpy(outName, inName); strcat(outName, e); }
	}
	else {
		const char* e = has_suffix(inName, ".wz") ? ".wz" : has_suffix(inName, ".wlz4") ? ".wlz4" : NULL;
		if (!e) {
			if (!isStdin) fclose(in);
			return fail("unknown suffix (.wz or .wlz4 expected; use -o or -c)");
		}
		outName = strdup(inName);
		if (outName) outName[strlen(inName) - strlen(e)] = 0;
	}
	if (!toStdout && !outName) { if (!isStdin) fclose(in); return fail("out of memory"); }
	if (toStdout && g.op == OP_COMPRESS && isatty(fileno(stdout)) && !g.force) {
		if (!isStdin) fclose(in);
		return fail("compressed data not written to a terminal (use -f, or redirect)");
	}
	FILE* out = toStdout ? stdout : open_out(outName);
	if (!out) { if (!isStdin) fclose(in); free(outName); return 0; }
	if (toStdout) set_binary(stdout);

	ok = g.op == OP_COMPRESS ? compress_stream(in, out, contentSize, &inTotal, &outTotal)
	                         : decompress_stream(in, out, &inTotal, &outTotal, NULL);
	if (out == stdout) { if (fflush(stdout) != 0) ok = fail("write error"); }
	else if (fclose(out) != 0) ok = fail("write error");
	if (!isStdin) fclose(in);
	if (!ok && outName) unlink(outName);                /* no partial output */
	if (ok && outName && !isStdin) copy_time(inName, outName);
	if (ok) {
		const double t = now() - t0;
		const unsigned long long raw = g.op == OP_COMPRESS ? inTotal : outTotal, cmp = g.op == OP_COMPRESS ? outTotal : inTotal;
		msg(3, "\r");
		msg(2, "%-24s: %6.2f%%  (%llu => %llu bytes, %s)  %.1f MB/s\n", cur, raw ? 100.0 * cmp / raw : 100.0,
		    g.op == OP_COMPRESS ? raw : cmp, g.op == OP_COMPRESS ? cmp : raw, outName ? outName : "(stdout)",
		    t > 0 ? raw / t / 1e6 : 0.0);
		if (g.rmSource && !isStdin && outName && unlink(inName) != 0) fail(strerror(errno));
	}
	free(outName);
	return ok;
}

/*------   Benchmark   ------*/
static int bench(const char* name)
{
	cur = name;
	FILE* f = fopen(name, "rb");
	if (!f) return fail(strerror(errno));
	Buf src = { 0 };
	size_t n = 0;
	for (;;) {
		if (!buf_reserve(&src, n + (1 << 20) + 1)) { fclose(f); free(src.p); return fail("out of memory"); }
		const size_t r = fread(src.p + n, 1, src.cap - n, f);
		if (r == 0) break;
		n += r;
	}
	fclose(f);
	const int last = g.benchEnd > g.level ? g.benchEnd : g.level;
	int ok = 1;
	for (int level = g.level; level <= last && ok; level++) {
		const WZF_params p = { g.codec, level, g.blockLog, 1, g.threads };    /* the codec alone: no checksum */
		const size_t cap = WZF_compressBound(n, &p);
		unsigned char* c = (unsigned char*)malloc(cap);
		unsigned char* d = (unsigned char*)malloc(n + 32);
		double bc = 1e30, bd = 1e30, t, total = 0;
		size_t cs = 0;
		if (!c || !d) { free(c); free(d); return fail("out of memory"); }
		do {                                            /* the best of the compressions in at least one second */
			const double t0 = now();
			cs = WZF_compress(c, cap, src.p, n, &p);
			t = now() - t0; total += t;
			if (t < bc) bc = t;
		} while (!WZF_isError(cs) && total < 1.0);
		if (WZF_isError(cs)) ok = fail(WZF_getErrorName(cs));
		else {
			total = 0;
			do {
				const double t0 = now();
				const size_t r = WZF_decompress(d, n + 32, c, cs);
				t = now() - t0; total += t;
				if (t < bd) bd = t;
				if (r != n || memcmp(d, src.p, n)) { ok = fail("the round trip failed"); break; }
			} while (total < 1.0);
			if (ok)
				printf("%s %3d  %-24s %12zu -> %12zu  %6.3f  %8.2f MB/s  %8.1f MB/s\n", g.codec ? "WLZ4" : "WZIP", level,
				       name, n, cs, cs ? (double)n / cs : 0.0, bc > 0 ? n / bc / 1e6 : 0.0, bd > 0 ? n / bd / 1e6 : 0.0);
			fflush(stdout);
		}
		free(c); free(d);
	}
	free(src.p);
	return ok;
}

/*------   Arguments   ------*/
static int read_number(const char** s)
{
	int v = 0;
	while (**s >= '0' && **s <= '9') v = v * 10 + (*(*s)++ - '0');
	return v;
}

static void version(void) { printf("%s %s (WZ frame format 0; WZIP and WLZ4 formats 1)\n", g.prog, WZF_versionString()); }

int main(int argc, char** argv)
{
	const char* base = argv[0];
	for (const char* p = argv[0]; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
	if (!strncmp(base, "wlz4", 4)) { g.codec = WZF_CODEC_WLZ4; g.level = -1; g.prog = "wlz4"; }
	if (!strncmp(base, "unwzip", 6) || !strncmp(base, "unwlz4", 6)) g.op = OP_DECOMPRESS;

	const char** files = (const char**)malloc((argc + 1) * sizeof(char*));
	int nFiles = 0, endOpts = 0;
	if (!files) return 1;
	for (int i = 1; i < argc; i++) {
		const char* a = argv[i];
		if (endOpts || a[0] != '-' || !a[1]) { files[nFiles++] = a; continue; }
		if (!strcmp(a, "--")) { endOpts = 1; continue; }
		if (a[1] == '-') {
			if (!strcmp(a, "--decompress") || !strcmp(a, "--uncompress")) g.op = OP_DECOMPRESS;
			else if (!strcmp(a, "--compress")) g.op = OP_COMPRESS;
			else if (!strcmp(a, "--test")) g.op = OP_TEST;
			else if (!strcmp(a, "--list")) g.op = OP_LIST;
			else if (!strcmp(a, "--stdout")) g.toStdout = 1;
			else if (!strcmp(a, "--force")) g.force = 1;
			else if (!strcmp(a, "--keep")) g.rmSource = 0;
			else if (!strcmp(a, "--rm")) g.rmSource = 1;
			else if (!strcmp(a, "--quiet")) g.verbosity--;
			else if (!strcmp(a, "--verbose")) g.verbosity++;
			else if (!strcmp(a, "--no-check")) g.noCheck = 1;
			else if (!strcmp(a, "--check")) g.noCheck = 0;
			else if (!strcmp(a, "--wzip")) { g.codec = WZF_CODEC_WZIP; if (!g.levelSet) g.level = 1; }
			else if (!strcmp(a, "--wlz4")) { g.codec = WZF_CODEC_WLZ4; if (!g.levelSet) g.level = -1; }
			else if (!strcmp(a, "--fast")) { g.codec = WZF_CODEC_WLZ4; g.level = -2; g.levelSet = 1; }
			else if (!strcmp(a, "--lazy")) { g.codec = WZF_CODEC_WLZ4; g.level = -1; g.levelSet = 1; }
			else if (!strncmp(a, "--block-log=", 12)) { const char* s = a + 12; g.blockLog = read_number(&s); }
			else if (!strncmp(a, "--threads=", 10)) { const char* s = a + 10; g.threads = read_number(&s); }
			else if (!strcmp(a, "--help")) { usage(stdout); return 0; }
			else if (!strcmp(a, "--version")) { version(); return 0; }
			else { msg(1, "%s: unknown option %s\n", g.prog, a); usage(stderr); return 1; }
			continue;
		}
		for (const char* s = a + 1; *s; ) {            /* aggregated short options: -dc, -9kf, -B20 */
			const char o = *s++;
			if (o >= '0' && o <= '9') { s--; g.level = read_number(&s); g.levelSet = 1; continue; }
			switch (o) {
			case 'd': g.op = OP_DECOMPRESS; break;
			case 'z': g.op = OP_COMPRESS; break;
			case 't': g.op = OP_TEST; break;
			case 'l': g.op = OP_LIST; break;
			case 'b': g.op = OP_BENCH; if (*s >= '0' && *s <= '9') { g.level = read_number(&s); g.levelSet = 1; } break;
			case 'e': g.benchEnd = read_number(&s); break;
			case 'c': g.toStdout = 1; break;
			case 'f': g.force = 1; break;
			case 'k': g.rmSource = 0; break;
			case 'q': g.verbosity--; break;
			case 'v': g.verbosity++; break;
			case 'B': g.blockLog = read_number(&s); break;
			case 'T': g.threads = read_number(&s); break;
			case 'h': usage(stdout); return 0;
			case 'V': version(); return 0;
			case 'o':
				if (*s) { g.outName = s; s += strlen(s); }
				else if (i + 1 < argc) g.outName = argv[++i];
				else { msg(1, "%s: -o needs a file name\n", g.prog); return 1; }
				break;
			default: msg(1, "%s: unknown option -%c\n", g.prog, o); usage(stderr); return 1;
			}
		}
	}
	if (g.codec == WZF_CODEC_WZIP && (g.level < 0 || g.level > 13)) { msg(1, "%s: WZIP levels are 0-13\n", g.prog); return 1; }
	if (g.codec == WZF_CODEC_WLZ4 && (g.level < -2 || g.level > 12)) { msg(1, "%s: WLZ4 levels are 0-12, --fast and --lazy\n", g.prog); return 1; }
	if (g.blockLog && (g.blockLog < WZF_BLOCKLOG_MIN || g.blockLog > WZF_BLOCKLOG_MAX)) { msg(1, "%s: -B takes 10-31\n", g.prog); return 1; }
	if (g.threads < 1 || g.threads > 256) { msg(1, "%s: -T takes 1-256\n", g.prog); return 1; }
	if (nFiles == 0) files[nFiles++] = "-";
	if (g.outName && nFiles > 1 && strcmp(g.outName, "-")) { msg(1, "%s: -o takes one input file\n", g.prog); return 1; }

	int failed = 0;
	if (g.op == OP_LIST)
		printf("%-30s %6s %7s  %-5s %3s  %-5s %14s %14s  %6s\n", "file", "frames", "blocks", "codec", "B", "check",
		       "compressed", "content", "ratio");
	for (int i = 0; i < nFiles; i++)
		if (!(g.op == OP_BENCH ? bench(files[i]) : process(files[i]))) failed = 1;
	free((void*)files);
	return failed;
}
