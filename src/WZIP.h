/*
 * WZIP - public interface
 * Copyright (c) 2018-present, Yingquan (Cody) Wu.
 * SPDX-License-Identifier: BSD-2-Clause (see LICENSE)
 */

#if defined (__cplusplus)
extern "C" {
#endif

#ifndef WZIP_H_198382716213
#define WZIP_H_198382716213

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>   /* size_t */

/* The compressed formats (the wzip_compress stream, WZIP_L, WZIP_M and WZIP_S) are specified in doc/WZIP_format.md. */

#define WZIP_MAX_INPUT_SIZE       0x7EEEE000
#define WZIP_MEM_OVERHEAD         256
#define WZIP_MAX_OFF_WIDTH        27          /* the window of lengths 8+ covers the input up to 2^27 bytes */
#define WZIP_SHORT_OFF_WIDTH      26          /* the windows of lengths 3-7 stop at 2^26 */


/* hash3Table and chain3Table are currently unutilized */
typedef struct  {
	int compressLevel;
	unsigned maxSearchCnt;
	unsigned curr1Idx, curr2Idx, curr3Idx;
	unsigned hash0Len, hash1Len, hash2Len;
	unsigned hash0Mask, hash1Mask, hash2Mask, hash3Mask;
	unsigned chain1Mask, chain2Mask, chain3Mask;
	void* hash0Table, *hash1Table, *hash2Table, *hash3Table;
	void *chain1Table, *chain2Table, *chain3Table;
	int dictSize;
	unsigned char* dictEnd;
} WZIP_State_Str;

void WZIP_Set_OffWidth(int srcSize, int* offWidth);

void WZIP_State_Load_Dict(WZIP_State_Str* dictStr, WZIP_State_Str* dstStr);

void WZIP_Free_State(WZIP_State_Str* wzipStr);

/* An output capacity with which wzip_compress never runs out of room: srcSize + WZIP_MEM_OVERHEAD */
int WZIP_Cap_CmprSize(int srcSize);

//It reads out the decompressed data size, stored ahead of the compression output (2 bytes below 32 KB, else 4;
//0: stored uncompressed), and reduces *srcSize by the header's length
int WZIP_Read_DecSize(const void* const source, int* srcSize);

WZIP_State_Str* WZIP_New_State_L(int level, int srcSize, const void* dict, int dictSize);
WZIP_State_Str* WZIP_New_State_M(int level, const void* dict, int dictSize);

int WZIP_Compress_L(WZIP_State_Str* wzipStr, const void* const source, int srcSize, void* const wzipStream, int wzipCapSize);
int WZIP_Compress_M(WZIP_State_Str* wzipStr, const void* const source, int srcSize, void* const wzipStream, int wzipCapSize);
int WZIP_Decompress_L(const void* const source, int const srcSize, void* const dest, int const destSize, void* const dict, int const dictSize);
int WZIP_Decompress_M(const void* const source, int const srcSize, void* const dest, int const destSize, void* const dict, int const dictSize);
/* trusted mode (see wzip_decompress_trusted): no checks */
int WZIP_Decompress_L_Trusted(const void* const source, int const srcSize, void* const dest, int const destSize, void* const dict, int const dictSize);
int WZIP_Decompress_M_Trusted(const void* const source, int const srcSize, void* const dest, int const destSize, void* const dict, int const dictSize);

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ One-call interface: WZIP_L for 32 KB and more, WZIP_M below ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

/* Compresses srcSize bytes (up to WZIP_MAX_INPUT_SIZE) at level 0-13 into wzipStream, whose capacity is *wzipCapSize;
   WZIP_Cap_CmprSize(srcSize) always suffices. Returns the compressed size, at most srcSize + 2 (an input that does not
   shrink is stored), or 0 if it does not fit or an argument is invalid. The caller's buffer is never reallocated. */
int wzip_compress(const void* const source, int srcSize, void* const wzipStream, int *wzipCapSize, int level);

/* Decompresses a stream of wzip_compress into decmp, whose capacity *decCapSize must hold the decoded size (which
   WZIP_Read_DecSize reports); nothing is written past it. Returns the decoded size, or 0 if the buffer is too small
   or the stream is corrupt or truncated: the input is validated, and no read leaves source[0, srcSize). Not
   thread-safe (WZIP_L and WZIP_M keep their window schedule in global state). */
int wzip_decompress(const void* const source, int srcSize, void* decmp, int *decCapSize);

/* Trusted mode, opt-in: the same decoding without any check, for input known to come unmodified from wzip_compress
   (e.g. data this program compressed, or verified by a cryptographic MAC). The stored size still sizes the output;
   the input must stay readable WZIP_TRUSTED_SRC_PAD bytes past srcSize. A damaged stream can make it read or write out
   of bounds. On Silesia it decodes up to about 8% faster than wzip_decompress; the paper's figures are this mode's. */
#define WZIP_TRUSTED_SRC_PAD      32
int wzip_decompress_trusted(const void* const source, int srcSize, void* decmp, int *decCapSize);

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ WZIP_S : short fixed-size blocks (e.g. 4K/8K storage pages) ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

#define WZIPS_MAX_BLOCK            32768                /* largest supported block, in bytes */
#define WZIPS_COMPRESSBOUND(n)     ((n) + 3)            /* worst-case compressed size of an n-byte block */

typedef struct WZIPS_CCtx_s WZIPS_CCtx;

/* A compression context holds all match-finder tables; create once and reuse across blocks. */
WZIPS_CCtx* WZIPS_createCCtx(void);
void        WZIPS_freeCCtx(WZIPS_CCtx* cctx);

/* Compresses srcSize bytes (1..WZIPS_MAX_BLOCK) at level 1..9.
   Returns the compressed size, or 0 if dstCap < WZIPS_COMPRESSBOUND(srcSize) or the arguments are invalid. */
int WZIPS_compress(WZIPS_CCtx* cctx, const void* src, int srcSize, void* dst, int dstCap, int level);

/* Returns the original size stored at the front of a compressed block, or a negative value if malformed. */
int WZIPS_getDecompressedSize(const void* src, int srcSize);

/* Decompresses a whole block. dstCap need only equal the original size: nothing is written past it.
   Returns the original size, or a negative value on malformed input or insufficient dstCap. */
int WZIPS_decompress(const void* src, int srcSize, void* dst, int dstCap);

/* Dictionary-assisted compression. The dictionary logically precedes each block, so matches may
   reach back into it; only its last WZIPS_MAX_DICT bytes are used. A prepared dictionary is built
   once, is only read while compressing, and may serve any number of contexts and blocks.
   A block compressed with a dictionary needs the same dictionary bytes to decompress. */
#define WZIPS_MAX_DICT             32767

typedef struct WZIPS_CDict_s WZIPS_CDict;

WZIPS_CDict* WZIPS_createCDict(const void* dict, int dictSize);
void         WZIPS_freeCDict(WZIPS_CDict* cdict);

/* As WZIPS_compress; cdict may be NULL */
int WZIPS_compress_usingCDict(WZIPS_CCtx* cctx, const WZIPS_CDict* cdict, const void* src, int srcSize, void* dst, int dstCap, int level);

/* As WZIPS_decompress; dict is required for blocks compressed with a dictionary and ignored otherwise */
int WZIPS_decompress_usingDict(const void* src, int srcSize, void* dst, int dstCap, const void* dict, int dictSize);
#endif

#if defined (__cplusplus)
}
#endif