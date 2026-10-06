/*
 * WZ frames: the container of WZIP and WLZ4 data (magic number, codec and format version, blocks, content size,
 * checksum), specified in doc/frame_format.md.
 * Copyright (c) 2026-present, Yingquan (Cody) Wu.
 * SPDX-License-Identifier: BSD-2-Clause (see LICENSE)
 *
 * Sizes are size_t, so content beyond a codec's 2 GB limit is stored as several blocks. Functions that return a
 * size_t return either a size or an error code: test it with WZF_isError(), name it with WZF_getErrorName(). The
 * frame decoders validate their input (they use the codecs' bounds-checked decoders) and verify the checksum.
 * The functions are thread-safe when each thread uses its own contexts.
 */
#ifndef WZFRAME_H_2026
#define WZFRAME_H_2026

#include <stddef.h>

#if defined (__cplusplus)
extern "C" {
#endif

#define WZF_VERSION_MAJOR    1                /* the library's version, as WZIP_VERSION_* in WZIP.h */
#define WZF_VERSION_MINOR    0
#define WZF_VERSION_RELEASE  0
#define WZF_VERSION_NUMBER   (WZF_VERSION_MAJOR * 10000 + WZF_VERSION_MINOR * 100 + WZF_VERSION_RELEASE)
#define WZF_VERSION_STRING   "1.0.0"
unsigned WZF_versionNumber(void);
const char* WZF_versionString(void);

#define WZF_MAGIC              0x0A5A578Du      /* bytes 8D 57 5A 0A */
#define WZF_SKIPPABLE_MIN      0x184D2A50u      /* skippable frames: magic 0x184D2A50-5F, u32 size, data */
#define WZF_HEADER_MIN         7                /* magic and descriptor */
#define WZF_HEADER_MAX         15               /* with the content size */
#define WZF_BLOCK_HEADER       4
#define WZF_BLOCKLOG_MIN       10
#define WZF_BLOCKLOG_MAX       31

enum { WZF_CODEC_WZIP = 0, WZF_CODEC_WLZ4 = 1 };

/*------   Errors   ------*/
typedef enum {
	WZF_error_none = 0,
	WZF_error_generic,
	WZF_error_parameter,                /* an invalid argument */
	WZF_error_memory,                   /* an allocation failed */
	WZF_error_dstSize_tooSmall,         /* the output buffer is too small */
	WZF_error_srcSize_wrong,            /* the input ends inside a frame */
	WZF_error_unknown_frame,            /* not a WZ frame (unknown magic number) */
	WZF_error_unsupported,              /* a codec, format version or flag this library does not implement */
	WZF_error_corrupted,                /* a malformed block or frame */
	WZF_error_checksum,                 /* the content checksum does not match */
	WZF_error_contentSize,              /* the content size does not match */
	WZF_error_maxCode
} WZF_ErrorCode;

unsigned WZF_isError(size_t code);
WZF_ErrorCode WZF_getErrorCode(size_t code);
const char* WZF_getErrorName(size_t code);

/*------   Parameters   ------*/
typedef struct {
	int codec;          /* WZF_CODEC_WZIP or WZF_CODEC_WLZ4 */
	int level;          /* WZIP: 0-13. WLZ4: -2 fast, -1 lazy, 0-7 hash chains, 8-12 optimal parsing */
	int blockLog;       /* blocks decode to at most 2^blockLog bytes (10-31); 0: by the content size (doc) */
	int noChecksum;     /* 1: no content checksum */
} WZF_params;

#define WZF_CONTENTSIZE_UNKNOWN (~0ULL)

/*------   One-shot   ------*/

/* The largest frame WZF_compress can produce for srcSize bytes. */
size_t WZF_compressBound(size_t srcSize, const WZF_params* params);

/* Compresses src into one frame in dst. Returns the frame's size or an error. */
size_t WZF_compress(void* dst, size_t dstCapacity, const void* src, size_t srcSize, const WZF_params* params);

/* The total content size of the frames in src, if every frame records it; WZF_CONTENTSIZE_UNKNOWN otherwise, or
   if src is not a sequence of frames (only headers are read, so a damaged frame may still fail to decode). */
unsigned long long WZF_getContentSize(const void* src, size_t srcSize);

/* Decompresses every frame in src (skipping skippable frames) into dst. Returns the content size or an error. */
size_t WZF_decompress(void* dst, size_t dstCapacity, const void* src, size_t srcSize);

/*------   Block by block (streaming)   ------
   Compression: WZF_compressBegin writes the frame header; WZF_compressBlock compresses one block of at most
   2^blockLog bytes (the caller chooses the cut) and writes its header and data; WZF_compressEnd writes the end mark
   and the checksum. Each returns the bytes written, or an error. A block writes at most WZF_BLOCK_HEADER + srcSize
   bytes and needs that much room; with WZF_blockBound(srcSize) it compresses in place, without an internal copy. */
typedef struct WZF_CCtx_s WZF_CCtx;
WZF_CCtx* WZF_createCCtx(void);
void      WZF_freeCCtx(WZF_CCtx* cctx);
size_t WZF_compressBegin(WZF_CCtx* cctx, void* dst, size_t dstCapacity, const WZF_params* params,
                         unsigned long long contentSize);       /* contentSize: WZF_CONTENTSIZE_UNKNOWN if unknown */
size_t WZF_compressBlock(WZF_CCtx* cctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);
size_t WZF_compressEnd(WZF_CCtx* cctx, void* dst, size_t dstCapacity);
size_t WZF_blockBound(size_t srcSize);
int    WZF_blockLog(const WZF_CCtx* cctx);                     /* the frame's block size log, after Begin */

/* Decompression: feed the frame in the pieces the functions ask for.
   1. WZF_decompressBegin(dctx, src, n) with the first n >= WZF_HEADER_MIN bytes of the frame returns the header's
      size h (7 or 15; or, for a skippable frame, 8 + its size, to be skipped) or an error; call it again with h
      bytes if h > n (the result is h again).
   2. Then repeatedly read WZF_BLOCK_HEADER bytes and pass them to WZF_nextBlock, which returns the size c of the
      block's data (0: the end mark) and sets *isRaw.
   3. For c > 0, read c bytes and pass them to WZF_decompressBlock, which decodes them into dst (room for
      WZF_frameBlockSize(dctx) bytes always suffices) and returns the block's decoded size or an error.
   4. After the end mark, WZF_endSize(dctx) is the number of bytes left (0 or 4, the checksum); pass them to
      WZF_decompressEnd, which checks the checksum and content size and returns 0 or an error. */
typedef struct WZF_DCtx_s WZF_DCtx;
typedef struct {
	unsigned long long contentSize;     /* WZF_CONTENTSIZE_UNKNOWN if not recorded */
	int codec;
	int codecVersion;
	int blockLog;
	int checksum;                       /* 1: the frame has a content checksum */
	int skippable;                      /* 1: a skippable frame (the other fields are 0) */
} WZF_FrameHeader;
WZF_DCtx* WZF_createDCtx(void);
void      WZF_freeDCtx(WZF_DCtx* dctx);
size_t WZF_decompressBegin(WZF_DCtx* dctx, const void* src, size_t srcSize);
const WZF_FrameHeader* WZF_frameHeader(const WZF_DCtx* dctx);
size_t WZF_frameBlockSize(const WZF_DCtx* dctx);               /* 2^blockLog */
size_t WZF_nextBlock(WZF_DCtx* dctx, const void* blockHeader, int* isRaw);
size_t WZF_decompressBlock(WZF_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);
/* The decoded size of the block announced by WZF_nextBlock, from the first min(4, c) bytes of its data (a codec
   stream begins with its size), so that a caller can size dst per block; or an error if the size is invalid. */
size_t WZF_blockDecodedSize(const WZF_DCtx* dctx, const void* src, size_t srcSize);
size_t WZF_endSize(const WZF_DCtx* dctx);
size_t WZF_decompressEnd(WZF_DCtx* dctx, const void* src, size_t srcSize);

/* XXH32 (doc/frame_format.md, section 8), as the frames use it */
unsigned WZF_XXH32(const void* src, size_t srcSize, unsigned seed);

#if defined (__cplusplus)
}
#endif
#endif
