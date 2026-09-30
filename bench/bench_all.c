/* One benchmark for all codecs of the paper: each file compressed whole (one call), single thread pinned to core 2,
   high priority. Ratio = total input / total output; speeds = total input / total time, compression best of
   <crounds>, decompression best of <drounds>; every file is verified. Encoder memory = peak working set during
   compression minus the working set before it (all buffers allocated and touched beforehand).
   Usage: bench_all <codec> <level> <crounds> <drounds> file...
   codecs: wzip (0-13), zstd (1-22), brotli (0-11, window 2^24), xz (0-9, add 100 for extreme), lz4 (acceleration),
           lz4hc (1-12), wlz4f (acceleration), wlz4l (lazy; level ignored), wlz4hc (0-12)
   WZIP and WLZ4 decode in their trusted mode, as in the paper (the compressed buffers have the required slack); env
   CHECKED=1 selects their default, bounds-checked decoders. LZ4 decodes with LZ4_decompress_safe.
   Env PERFILE=1 prints each file's compressed size.
   Env STREAMS=<dir>: decompression only, from compressed streams saved in <dir> (<file>.<codec><level>); a missing
   stream is compressed (untimed) and saved first. The compression speed and memory then print as 0; with
   STREAMS_ONLY=1 the program only prepares the streams, unpinned, so that several can be prepared in parallel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>
#include <zstd.h>
#include <brotli/encode.h>
#include <brotli/decode.h>
#include <lzma.h>
#include <lz4.h>
#include <lz4hc.h>
#include "WZIP.h"
#include "WLZ4.h"

static double now(void) { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart / f.QuadPart; }
static double wsMB(int peak)
{
    PROCESS_MEMORY_COUNTERS pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    return (peak ? pmc.PeakWorkingSetSize : pmc.WorkingSetSize) / 1048576.0;
}

static const char* codec;
static int level;
static int checked;                                      /* CHECKED=1: the bounds-checked WZIP and WLZ4 decoders */
static ZSTD_CCtx* zc; static ZSTD_DCtx* zd;
static WLZ_State_Str* ws; static WLZhc_State_Str* hs;

static size_t compress1(const unsigned char* src, size_t n, unsigned char* dst, size_t cap)
{
    if (!strcmp(codec, "wzip")) { int c = (int)cap; return (size_t)wzip_compress(src, (int)n, dst, &c, level); }
    if (!strcmp(codec, "zstd")) return ZSTD_compressCCtx(zc, dst, cap, src, n, level);
    if (!strcmp(codec, "brotli")) { size_t out = cap; return BrotliEncoderCompress(level, 24, BROTLI_MODE_GENERIC, n, src, &out, dst) ? out : 0; }
    if (!strcmp(codec, "xz")) {
        size_t pos = 0;
        const uint32_t preset = (level % 100) | (level >= 100 ? LZMA_PRESET_EXTREME : 0);
        return lzma_easy_buffer_encode(preset, LZMA_CHECK_NONE, NULL, src, n, dst, &pos, cap) == LZMA_OK ? pos : 0;
    }
    if (!strcmp(codec, "lz4")) return (size_t)LZ4_compress_fast((const char*)src, (char*)dst, (int)n, (int)cap, level);
    if (!strcmp(codec, "lz4hc")) return (size_t)LZ4_compress_HC((const char*)src, (char*)dst, (int)n, (int)cap, level);
    if (!strcmp(codec, "wlz4f")) return WLZ_Compress_Fast(ws, (const char*)src, (char*)dst, (unsigned)n, (unsigned)cap, level);
    if (!strcmp(codec, "wlz4l")) return WLZ_Compress(ws, (const char*)src, (char*)dst, (unsigned)n, (unsigned)cap);
    if (!strcmp(codec, "wlz4hc")) return WLZhc_Compress(hs, (const char*)src, (char*)dst, (unsigned)n, (unsigned)cap, level);
    fprintf(stderr, "unknown codec %s\n", codec); exit(1);
}

static size_t decompress1(const unsigned char* src, size_t cs, unsigned char* dst, size_t cap)
{
    if (!strcmp(codec, "wzip")) { int c = (int)cap; return (size_t)(checked ? wzip_decompress(src, (int)cs, dst, &c) : wzip_decompress_trusted(src, (int)cs, dst, &c)); }
    if (!strcmp(codec, "zstd")) return ZSTD_decompressDCtx(zd, dst, cap, src, cs);
    if (!strcmp(codec, "brotli")) { size_t out = cap; return BrotliDecoderDecompress(cs, src, &out, dst) == BROTLI_DECODER_RESULT_SUCCESS ? out : 0; }
    if (!strcmp(codec, "xz")) {
        uint64_t memlimit = UINT64_MAX; size_t inpos = 0, outpos = 0;
        return lzma_stream_buffer_decode(&memlimit, 0, NULL, src, &inpos, cs, dst, &outpos, cap) == LZMA_OK ? outpos : 0;
    }
    if (!strncmp(codec, "lz4", 3)) return (size_t)LZ4_decompress_safe((const char*)src, (char*)dst, (int)cs, (int)cap);
    return checked ? WLZ_Decompress((const char*)src, (char*)dst, (unsigned)cs, (unsigned)cap)
                   : WLZ_Decompress_Trusted((const char*)src, (char*)dst, (unsigned)cs, (unsigned)cap);
}

static const char* base_name(const char* p)
{
    const char* s = strrchr(p, '/'), *t = strrchr(p, '\\');
    if (t > s) s = t;
    return s ? s + 1 : p;
}

int main(int argc, char** argv)
{
    const char* const streams = getenv("STREAMS");
    const int streamsOnly = streams && getenv("STREAMS_ONLY") != NULL;
    if (!streamsOnly) {
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        SetThreadAffinityMask(GetCurrentThread(), 1 << 2);
    }
    codec = argv[1]; level = atoi(argv[2]);
    checked = getenv("CHECKED") != NULL;
    const int crounds = atoi(argv[3]), drounds = atoi(argv[4]), nf = argc - 5;
    const int perFile = getenv("PERFILE") != NULL;
    unsigned char** src = malloc(nf * sizeof(*src)), **cmp = malloc(nf * sizeof(*cmp));
    size_t* n = malloc(nf * sizeof(size_t)), *cs = malloc(nf * sizeof(size_t)), maxN = 0;
    for (int f = 0; f < nf; f++) {
        FILE* fp = fopen(argv[f + 5], "rb");
        if (!fp) { printf("cannot open %s\n", argv[f + 5]); return 1; }
        fseek(fp, 0, SEEK_END); n[f] = (size_t)ftell(fp); fseek(fp, 0, SEEK_SET);
        src[f] = malloc(n[f] + 64); cmp[f] = malloc(n[f] + n[f] / 8 + 65536);
        if (fread(src[f], 1, n[f], fp) != n[f]) return 1;
        fclose(fp);
        memset(cmp[f], 0, n[f] + n[f] / 8 + 65536);
        if (n[f] > maxN) maxN = n[f];
    }
    unsigned char* dec = malloc(maxN + 65536);
    memset(dec, 0, maxN + 65536);
    zc = ZSTD_createCCtx(); zd = ZSTD_createDCtx();
    ws = WLZ_New_State(); hs = WLZhc_New_State();

    {   /* warm-up: one second of work, so that timing starts at full clock speed */
        volatile unsigned long long x = 0;
        const double t0 = now();
        while (now() - t0 < 1.0) for (int i = 0; i < 100000; i++) x += i;
    }
    const double before = wsMB(0);
    double bestC = 1e30, total = 0, ctotal = 0;
    if (streams) {                                       /* saved streams: decompression only */
        for (int f = 0; f < nf; f++) {
            char path[4096];
            snprintf(path, sizeof path, "%s/%s.%s%d", streams, base_name(argv[f + 5]), codec, level);
            FILE* fp = fopen(path, "rb");
            if (fp) {
                fseek(fp, 0, SEEK_END); cs[f] = (size_t)ftell(fp); fseek(fp, 0, SEEK_SET);
                if (fread(cmp[f], 1, cs[f], fp) != cs[f]) return 1;
                fclose(fp);
            }
            else {
                cs[f] = compress1(src[f], n[f], cmp[f], n[f] + n[f] / 8 + 65536);
                if (NULL == (fp = fopen(path, "wb")) || fwrite(cmp[f], 1, cs[f], fp) != cs[f]) { printf("cannot write %s\n", path); return 1; }
                fclose(fp);
            }
            total += n[f]; ctotal += cs[f];
        }
        if (streamsOnly) return 0;
    }
    for (int r = 0; r < (streams ? 0 : crounds); r++) {
        double t = 0;
        total = ctotal = 0;
        for (int f = 0; f < nf; f++) {
            const double t0 = now();
            cs[f] = compress1(src[f], n[f], cmp[f], n[f] + n[f] / 8 + 65536);
            t += now() - t0;
            total += n[f]; ctotal += cs[f];
        }
        if (t < bestC) bestC = t;
    }
    const double encMem = streams ? 0 : wsMB(1) - before;
    if (streams) bestC = 1e300;                          /* prints 0: not measured */

    double bestD = 1e30;
    int fails = 0;
    for (int r = 0; r < drounds; r++) {
        double t = 0;
        for (int f = 0; f < nf; f++) {
            const double t0 = now();
            const size_t d = decompress1(cmp[f], cs[f], dec, maxN + 65536);
            t += now() - t0;
            if (r == 0 && (d != n[f] || memcmp(dec, src[f], n[f]))) fails++;
        }
        if (t < bestD) bestD = t;
    }
    if (perFile) for (int f = 0; f < nf; f++) {
        const char* b = strrchr(argv[f + 5], '/') ? strrchr(argv[f + 5], '/') + 1 : argv[f + 5];
        printf("  %-14s %10zu -> %10zu  %.3f\n", b, n[f], cs[f], (double)n[f] / cs[f]);
    }
    printf("%-7s %3d  ratio %.4f  comp %8.2f MB/s  dec %8.1f MB/s  encmem %7.1f MB  fails %d\n", codec, level,
           total / ctotal, total / bestC / 1e6, total / bestD / 1e6, encMem, fails);
    return fails != 0;
}
