/*
 * WLZ4 as of 2026-09-29, the two-window format of the paper's Table 5 (row 2): one-byte offsets
 * (256 B) for lengths 3-4 and two-byte offsets (64 KB). Kept for the ablation only; src/WLZ4.c is the
 * released, flagged format.
 * Copyright (c) 2019-present, Yingquan (Cody) Wu.
 * SPDX-License-Identifier: BSD-2-Clause (see LICENSE)
 * Portions derived from LZ4, Copyright (c) 2011-present Yann Collet (BSD 2-Clause); see NOTICE.
 */

/*
 * ACCELERATION_DEFAULT :
 * Select "acceleration" for WLZ_compress_fast() when parameter value <= 0
 */
#define ACCELERATION_DEFAULT 1
#define WLZ_GAIN_THRESHOLD   -4

#define WLZ_HASH1_MASK   ((1<<WLZ_HASH1BITS)-1) 
#define WLZ_HASH2_MASK   ((1<<WLZ_HASH2BITS)-1) 
#define WLZhc_HASH1_MASK   ((1<<WLZhc_HASH1BITS)-1) 
#define WLZhc_HASH2_MASK   ((1<<WLZhc_HASH2BITS)-1) 

#define WLZ_MATCH1_WINDOW  (1<<8)
#define WLZ_MATCH2_WINDOW  (1<<16)

 /*-************************************
 *  Error detection
 **************************************/

//#define WLZ_DEBUG

#if defined(WLZ_DEBUG)
#  include <assert.h>
#else
#  ifndef assert
#    define assert(condition) ((void)0)
#  endif
#endif

#define WLZ_STATIC_ASSERT(c)   { enum { WLZ_static_assert = 1/(int)(!!(c)) }; }   /* use after variable declarations */

/*-************************************
*  CPU Feature Detection
**************************************/
/* WLZ_MEMORY_ACCESS
 * By default, access to unaligned memory is controlled by `memcpy()`, which is safe and portable.
 * Unfortunately, on some target/compiler combinations, the generated assembly is sub-optimal.
 * The below switch allow to select different access method for improved performance.
 * Method 0 (default) : use `memcpy()`. Safe and portable.
 * Method 1 : `__packed` statement. It depends on compiler extension (ie, not portable).
 *            This method is safe if your compiler supports it, and *generally* as fast or faster than `memcpy`.
 * Method 2 : direct access. This method is portable but violate C standard.
 *            It can generate buggy code on targets which assembly generation depends on alignment.
 *            But in some circumstances, it's the only known way to get the most performance (ie GCC + ARMv6)
 * See https://fastcompression.blogspot.fr/2015/08/accessing-unaligned-memory.html for details.
 * Prefer these methods in priority order (0 > 1 > 2)
 */
#ifndef WLZ_MEMORY_ACCESS   /* can be defined externally */
#  if defined(__GNUC__) && \
  ( defined(__ARM_ARCH_6__) || defined(__ARM_ARCH_6J__) || defined(__ARM_ARCH_6K__) \
  || defined(__ARM_ARCH_6Z__) || defined(__ARM_ARCH_6ZK__) || defined(__ARM_ARCH_6T2__) )
#    define WLZ_MEMORY_ACCESS 2
#  elif (defined(__INTEL_COMPILER) && !defined(_WIN32)) || defined(__GNUC__)
#    define WLZ_MEMORY_ACCESS 1
#  endif
#endif

/*
 * FORCE_SW_BITCOUNT
 * Define this parameter if your target system or compiler does not support hardware bit count
 */
#if defined(_MSC_VER) && defined(_WIN32_WCE)   /* Visual Studio for WinCE doesn't support Hardware bit count */
#  define FORCE_SW_BITCOUNT
#endif



/*-************************************
*  Dependency
**************************************/
/*
 * WLZ_SRC_INCLUDED:
 * Amalgamation flag, whether WLZ.c is included
 */
#ifndef WLZ_SRC_INCLUDED
#  define WLZ_SRC_INCLUDED 1
#endif

#ifndef WLZ_STATIC_LINKING_ONLY
#define WLZ_STATIC_LINKING_ONLY
#endif

#include "WLZ4.h"
#include "Memry.h"

/*-************************************
*  Compiler Options
**************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  include <intrin.h>
#  pragma warning(disable : 4127)        /* disable: C4127: conditional expression is constant */
#  pragma warning(disable : 4293)        /* disable: C4293: too large shift (32-bits) */
#endif  /* _MSC_VER */


#include <stdio.h>
#include <stdlib.h>   /* malloc, calloc, free */
#include <string.h>   /* memset, memcpy */

#undef MIN
#define MIN(a,b)    ( (a) < (b) ? (a) : (b) )
#ifndef max
#  define max(a,b)  ( (a) > (b) ? (a) : (b) )
#endif
#ifndef min
#  define min(a,b)  ( (a) < (b) ? (a) : (b) )
#endif


static const reg_t decRepreca[8] = { 0, 0x101010101010101, 0x1000100010001, 0x1000001000001, 0x100000001, 0x10000000001, 0x1000000000001, 0x100000000000001 };
static const reg_t decReprecaB[8] = { 0, 0x101010101010101, 0x1000100010001, 0x10000010000, 0x100000001, 0x1000000, 0x10000, 0x100 };
static const int decShiftB[8] = { 0, 64, 64, 48, 64, 40, 48, 56 };
static const int decOffset[8] = { 0, 0, 0, 2, 0, 3, 2, 1 };

/* WLZ_FAST_DEC_LOOP: LZ4-derived copy helpers, unused by the decoder below, which need symbols Memry.h no longer
   provides; off unless asked for */
#ifndef WLZ_FAST_DEC_LOOP
#  define WLZ_FAST_DEC_LOOP 0
#endif

#if WLZ_FAST_DEC_LOOP

WLZ_O2_INLINE_GCC_PPC64LE void
WLZ_memcpy_using_offset_base(Uint8* destPtr, const Uint8* srcPtr, Uint8* dstEnd, const int offset)
{
    if (offset < 8) {
        destPtr[0] = srcPtr[0];
        destPtr[1] = srcPtr[1];
        destPtr[2] = srcPtr[2];
        destPtr[3] = srcPtr[3];
        srcPtr += inc32table[offset];
        memcpy(destPtr+4, srcPtr, 4);
        srcPtr -= dec64table[offset];
        destPtr += 8;
    } else {
        memcpy(destPtr, srcPtr, 8);
        destPtr += 8;
        srcPtr += 8;
    }

    MemWildCpy8(destPtr, srcPtr, dstEnd);
}

/* customized variant of memcpy, which can overwrite up to 32 bytes beyond dstEnd
 * this version copies two times 16 bytes (instead of one time 32 bytes)
 * because it must be compatible with offsets >= 16. */
WLZ_O2_INLINE_GCC_PPC64LE void
MemWildCpy4(void* destPtr, const void* srcPtr, void* dstEnd)
{
    Uint8* d = (Uint8*)destPtr;
    const Uint8* s = (const Uint8*)srcPtr;
    Uint8* const e = (Uint8*)dstEnd;

    do { memcpy(d,s,16); memcpy(d+16,s+16,16); d+=32; s+=32; } while (d<e);
}

WLZ_O2_INLINE_GCC_PPC64LE void
WLZ_memcpy_using_offset(Uint8* destPtr, const Uint8* srcPtr, Uint8* dstEnd, const int offset)
{
    Uint8 v[8];
    switch(offset) {
    case 1:
        memset(v, *srcPtr, 8);
        goto copy_loop;
    case 2:
        memcpy(v, srcPtr, 2);
        memcpy(&v[2], srcPtr, 2);
        memcpy(&v[4], &v[0], 4);
        goto copy_loop;
    case 4:
        memcpy(v, srcPtr, 4);
        memcpy(&v[4], srcPtr, 4);
        goto copy_loop;
    default:
        WLZ_memcpy_using_offset_base(destPtr, srcPtr, dstEnd, offset);
        return;
    }

 copy_loop:
    memcpy(destPtr, v, 8);
    destPtr += 8;
    while (destPtr < dstEnd) {
        memcpy(destPtr, v, 8);
        destPtr += 8;
    }
}
#endif


/*-************************************
*  Common Constants
**************************************/

#define MIN_MATCH_LEN  3
#define MAX_HASH_LEN   5  


#define WILDCOPYLENGTH 8
#define MATCH_SAFEGUARD_DISTANCE  ((2*WILDCOPYLENGTH) - MIN_MATCH_LEN)   /* ensure it's possible to write 2 x wildcopyLength without overflowing output buffer */
#define FASTLOOP_SAFE_DISTANCE 64


#ifndef WLZ_MAX_DIST   /* can be user - defined at compile time */
#  define WLZ_MAX_DIST     (WLZ_MATCH2_WINDOW-1)
#endif

#define ML_BITS  4
#define ML_MASK  ((1U<<ML_BITS)-1)
#define RUN_BITS (8-ML_BITS)
#define RUN_MASK ((1U<<RUN_BITS)-1)


/*-************************************
*  Common functions
**************************************/



#ifndef WLZ_COMMONDEFS_ONLY
/*-************************************
*  Local Constants
**************************************/
static const Uint32 WLZ_skipTrigger = 6;  /* Increase this value ==> compression run slower on incompressible data */


/*-************************************
*  Local Utils
**************************************/
int WLZ_versionNumber(void) { return WLZ_VERSION_NUMBER; }
const char* WLZ_versionString(void) { return WLZ_VERSION_STRING; }

/*-******************************
*  Compression functions
********************************/



/* multiplicative hashes of the 3 (Hash1) and 5 (Hash2) bytes at stream, reduced to 'bits': the bytes sit at the
   top of a 64-bit word, so the top bits of the product depend on all of them; one multiply and one shift */
#define WLZ_HASH_PRIME   0x9E3779B185EBCA87ULL

ForceInlineTemplate Uint32 WLZ_Hash1(const Uint8* const stream, const Uint32 bits)
{
	return (Uint32)((((Uint64)MemReadLE4(stream)) << 40) * WLZ_HASH_PRIME >> (64 - bits));
}

ForceInlineTemplate Uint32 WLZ_Hash2(const Uint8* const stream, const Uint32 bits)
{
	return (Uint32)((MemReadLE8(stream) << 24) * WLZ_HASH_PRIME >> (64 - bits));
}


#define WLZ_WRITE_ExtraLength(destPtr, len) {         \
	if (likely (len <252) ) *destPtr++ = (Uint8)len;  \
	else {                                            \
		len -= 252;                                   \
		int n = 1 + (len > ByteMask[1]) + (len > ByteMask[2]) + (len > ByteMask[3]); \
		*destPtr++ = (Uint8)(251 + n);                \
		MemWriteLE4(destPtr, len);           \
		destPtr += n;                                 \
	}                                                 \
}

/* bytes of a length extension (a literal run, or a match-length code, from 15 on) of value v */
ForceInlineTemplate int WLZ_Ext_Size(const Uint32 v)
{
	return v < 252 ? 1 : 2 + (v - 252 > 0xFF) + (v - 252 > 0xFFFF) + (v - 252 > 0xFFFFFF);
}

/* bytes of a sequence: token, literal run, offset (one byte for lengths 3-4, else two), match-length extension */
ForceInlineTemplate int WLZ_Seq_Size(const Uint32 litLen, const Uint32 matchLen)
{
	return 1 + (int)litLen + (litLen >= RUN_MASK ? WLZ_Ext_Size(litLen - RUN_MASK) : 0) + (matchLen < MAX_HASH_LEN ? 1 : 2)
	     + (matchLen - MIN_MATCH_LEN >= ML_MASK ? WLZ_Ext_Size(matchLen - MIN_MATCH_LEN - ML_MASK) : 0);
}

/* a sequence is written only if the output stays within -WLZ_GAIN_THRESHOLD bytes of the input covered: in
   incompressible data a short match after a long literal run costs more than it saves, and its bytes stay literals.
   This bounds the output (input size + a few bytes) without giving up on the rest of the input. */
#define WLZ_SEQ_PAYS(outBytes, seqBytes, inBytes)   ((int)(outBytes) + (int)(seqBytes) + WLZ_GAIN_THRESHOLD <= (int)(inBytes))

ForceInlineTemplate int WLZ_Match_Count(Uint8 *srcPtr, Uint8* matchPtr, const Uint8 const *srcLimit, const Uint8* const matchLimit)
{
	int matchLen = 0;
	reg_t matchDiff;

	matchDiff = MemReadARCH(matchPtr) ^ MemReadARCH(srcPtr);
	while (0 == matchDiff && srcPtr < srcLimit && (matchLimit == NULL || matchPtr < matchLimit) ) {
		srcPtr += REG_SIZE;
		matchPtr += REG_SIZE;
		matchLen += REG_SIZE;
		matchDiff = MemReadARCH(matchPtr) ^ MemReadARCH(srcPtr);
	} 

	matchLen += matchDiff==0? REG_SIZE : N_ZeroBytes(matchDiff);

	return matchLen;
}




/****************************************************************************************************************************************************************************/

/** forced inline, to ensure constant branches are decided at compilation time **/
/* the decoded size, ahead of the block: 2 bytes below 32 KB, else 4 (the first two carry the flag in the top bit and
   the low 15 bits, the next two the high bits), so a decoder can size its output from the first bytes */
ForceInlineTemplate Uint8* WLZ_Write_Size(Uint8* destPtr, const Uint32 srcSize)
{
	if (srcSize >> 15) {
		MemWriteLE2(destPtr, (Uint16)((1 << 15) | (srcSize & ((1 << 15) - 1))));
		MemWriteLE2(destPtr + 2, (Uint16)(srcSize >> 15));
		return destPtr + 4;
	}
	MemWriteLE2(destPtr, (Uint16)srcSize);
	return destPtr + 2;
}

ForceInlineTemplate Uint32 WLZ_Compress_Kernel(
                 WLZ_State_Str* const wlzStr,
                 const char* const source,
                 char* const destiny,
				 const int srcSize,
                 int acceleration)
{
	unsigned i;
    const Uint8* srcPtr = (const Uint8*) source;
    const Uint8* anchor = (const Uint8*) source;
    const Uint8* const srcEnd = (const Uint8*)source + srcSize;
    const Uint8* const srcLastMatch = srcEnd - REG_SIZE*2;
    Uint8* destPtr = (Uint8*) destiny;
	Uint32 hashV1, hashV2;
	int  match1Idx, match2Idx;
	Uint32  lazyMatchLen, lazyMatchOffset, lazyMatchFail;
	Uint32 matchLen, matchOffset;
	Uint32 litLen, extraLitLen;
	int *hash1Table = wlzStr->hash1Table;
	int *hash2Table = wlzStr->hash2Table;
	const Uint32 dictSize = wlzStr->dictSize;
	const Uint8* const dictEnd = wlzStr->dictEnd;
	const Uint8* const dictLastMatch = dictSize ? dictEnd - REG_SIZE * 2 : NULL;
	reg_t currPattern, diffPattern;

	const Uint8 *matchPtr;
	Uint8* token;
	const Uint32 isLazyMatch = (acceleration == 0);         // acceleration=0 indicates lazy match

#ifdef WLZ_DEBUG 
	FILE *fptr = fopen("WLZ_Compress_Index.txt", "w");
	fprintf(fptr, "WLZ_Compress_Kernel: srcSize=%i\n", srcSize);
	int matchStat[ML_MASK+1] = { 0 };
#endif
    
    /* If init conditions are not met, we don't have to mark stream
     * as having dirty context, since no action was taken yet */
    if ((Uint32)srcSize > (Uint32)WLZ_MAX_INPUT_SIZE) return 0;           /* Unsupported srcSize, too large (or negative) */
	destPtr = WLZ_Write_Size(destPtr, (Uint32)srcSize);
	Uint8* const payload = destPtr;                         /* the guard counts the payload only */
	acceleration = max(acceleration, 1);

    if ( srcSize <= MAX_HASH_LEN ) goto _last_literals;        /* Input too small, no compression (all literals) */

	Uint32 lastOffset = 1;
	Uint32 srcIdx = 1;
	srcPtr++;
	while( 1 ) {
        
        int step = 1; 
        int searchMatchNb = acceleration << WLZ_skipTrigger;
        while( 1 )  {
			/*if (srcIdx == 4194292) {
				srcIdx += 0;
			}*/

            if ( unlikely(srcPtr >= srcLastMatch) ) goto _last_literals;
			
			hashV1 = WLZ_Hash1(srcPtr, WLZ_HASH1BITS);  
			hashV2 = WLZ_Hash2(srcPtr, WLZ_HASH2BITS);
			match2Idx = hash2Table[ hashV2 ];
			hash2Table[ hashV2 ] = srcIdx;
			match1Idx = hash1Table[ hashV1 ];
			hash1Table[ hashV1 ] = srcIdx;		
			currPattern = MemReadARCH(srcPtr);
			matchLen = 0;
			matchOffset = srcIdx - match2Idx;
			if ( match2Idx && matchOffset - 1 < WLZ_MATCH2_WINDOW - 1 ) {      /* 1 <= offset < window */			
				matchPtr = (dictSize && match2Idx < 0) ? dictEnd+match2Idx : srcPtr - matchOffset;
				diffPattern = currPattern^ MemReadARCH(matchPtr);
				if (0==diffPattern) {
					matchLen = REG_SIZE + WLZ_Match_Count(srcPtr + REG_SIZE, matchPtr + REG_SIZE, srcLastMatch, dictLastMatch);
					break;
				}

				matchLen = N_ZeroBytes(diffPattern);
				if ( matchLen >= MAX_HASH_LEN )
					break;
			}
			/*if (matchOffset != lastOffset) {
				matchOffset = lastOffset;
				match2Idx = srcIdx - matchOffset;
				matchPtr = (dictSize && match2Idx < 0) ? dictEnd + match2Idx : srcPtr - matchOffset;
				if (MemRead4(srcPtr) == MemRead4(matchPtr)) {
					matchLen = 4 + WLZ_Match_Count(srcPtr + 4, matchPtr + 4, srcLastMatch, dictLastMatch);
					if (matchLen >= MAX_HASH_LEN || matchOffset < 256) 
						break;
				}
			}*/

			matchOffset = srcIdx - match1Idx;
			if (match1Idx && matchOffset - 1 < WLZ_MATCH2_WINDOW - 1 ) {
				matchPtr = (dictSize && match1Idx < 0) ? dictEnd+match1Idx : srcPtr - matchOffset;
				diffPattern = currPattern ^ MemReadARCH(matchPtr);
				if (0 == diffPattern) {
					matchLen = REG_SIZE + WLZ_Match_Count(srcPtr + REG_SIZE, matchPtr + REG_SIZE, srcLastMatch, dictLastMatch);
					break;
				}
				matchLen = N_ZeroBytes(diffPattern);
				if (matchLen >= MAX_HASH_LEN || ( matchOffset < WLZ_MATCH1_WINDOW && matchLen>= MIN_MATCH_LEN) )
					break;
			}
			
			srcPtr += step;
			srcIdx += step;
			step = (searchMatchNb++ >> WLZ_skipTrigger);
        } 
				
		if (isLazyMatch) {
			lazyMatchLen = 0;
			lazyMatchFail = 1;
			srcPtr++;
			srcIdx++;

			lazyMatchOffset = lastOffset;
			match2Idx = srcIdx - lazyMatchOffset;
			matchPtr = (dictSize && match2Idx < 0) ? dictEnd + match2Idx : srcPtr - lazyMatchOffset;
			if (MemRead4(srcPtr) == MemRead4(matchPtr)) {
				lazyMatchLen = 4 + WLZ_Match_Count(srcPtr + 4, matchPtr + 4, srcLastMatch, dictLastMatch);
				if (lazyMatchLen > matchLen && (lazyMatchLen >= MAX_HASH_LEN || lazyMatchOffset < 256)) {
					matchLen = lazyMatchLen;
					matchOffset = lazyMatchOffset;
					lazyMatchFail = 0;
				}
			}

			hashV1 = WLZ_Hash1(srcPtr, WLZ_HASH1BITS);
			hashV2 = WLZ_Hash2(srcPtr, WLZ_HASH2BITS);
			match2Idx = hash2Table[hashV2];
			hash2Table[hashV2] = srcIdx;
			hash1Table[hashV1] = srcIdx;
			currPattern = MemReadARCH(srcPtr);
			lazyMatchOffset = srcIdx - match2Idx;
			if (match2Idx && lazyMatchOffset - 1 < WLZ_MATCH2_WINDOW - 1 && lazyMatchOffset != lastOffset ) {
				matchPtr = (dictSize && match2Idx < 0) ? dictEnd + match2Idx : srcPtr - lazyMatchOffset;
				diffPattern = currPattern ^ MemReadARCH(matchPtr);
				
				if (0 == diffPattern) {
					lazyMatchLen = REG_SIZE + WLZ_Match_Count(srcPtr + REG_SIZE, matchPtr + REG_SIZE, srcLastMatch, dictLastMatch);
				}
				else lazyMatchLen = N_ZeroBytes(diffPattern);

				if ( (matchLen!= MAX_HASH_LEN-1 && lazyMatchLen > matchLen&& lazyMatchLen >= MAX_HASH_LEN) ||  (matchLen== MAX_HASH_LEN-1 && lazyMatchLen>= MAX_HASH_LEN +1) ) {
					matchLen = lazyMatchLen;
					matchOffset = lazyMatchOffset;
					lazyMatchFail = 0;
				}
			}

			srcIdx -= lazyMatchFail;
			srcPtr -= lazyMatchFail;
		}
				
		if (matchOffset > 1) {
			i = 1 + isLazyMatch;
			hashV1 = WLZ_Hash1(srcPtr + i, WLZ_HASH1BITS);
			hashV2 = WLZ_Hash2(srcPtr + i, WLZ_HASH2BITS);
			hash1Table[ hashV1 ] = srcIdx + i;
			hash2Table[ hashV2 ] = srcIdx + i;

			for (++i; i < matchLen-1; i+=2) {
				hashV1 = WLZ_Hash1(srcPtr + i, WLZ_HASH1BITS);
				hashV2 = WLZ_Hash2(srcPtr + i, WLZ_HASH2BITS);
				hash1Table[hashV1] = srcIdx + i;
				hash2Table[hashV2] = srcIdx + i;
				hashV2 = WLZ_Hash2(srcPtr + i+1, WLZ_HASH2BITS);
								hash2Table[hashV2] = srcIdx + i+1;
			}
		}

		litLen = (Uint32)(srcPtr - anchor);
		if (!WLZ_SEQ_PAYS(destPtr - payload, WLZ_Seq_Size(litLen, matchLen), srcPtr + matchLen - (const Uint8*)source)) {
			srcPtr++;                                           /* costs more than it saves: a literal, and search on */
			srcIdx++;
			matchLen = 0;
			continue;
		}

        /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~   Encode Literal Run ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

       token = destPtr++;
       if (litLen >= RUN_MASK) {
            extraLitLen = litLen - RUN_MASK;
            *token = (RUN_MASK<<ML_BITS);
			WLZ_WRITE_ExtraLength(destPtr, extraLitLen);
			MemWildCopy(destPtr+16, anchor + 16, destPtr + litLen);
        }
        else *token = (Uint8)(litLen<<ML_BITS);

	    memcpy(destPtr, anchor, 16);
        destPtr += litLen;

#ifdef WLZ_DEBUG
		matchStat[min(ML_MASK, matchLen - MIN_MATCH_LEN)]++;
		fprintf(fptr, "srcIdx=%d, destIdx=%d,  litLen=%d,  matchLen=%d, matchOffset=%d\n",
			(int)(anchor - (const Uint8*)source), (int)(destPtr - (Uint8*)destiny), litLen, matchLen, matchOffset);
		fflush(fptr);
#endif
        
		/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~   Encode Match Pair  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
		srcIdx += matchLen;
		srcPtr += matchLen;

		matchLen -= MIN_MATCH_LEN;
		MemWriteLE2(destPtr, (Uint16)matchOffset);
		destPtr += matchLen < MAX_HASH_LEN - MIN_MATCH_LEN ? 1 : 2;
		lastOffset = matchOffset;

		if (matchLen >= ML_MASK) {
            *token += ML_MASK;
			matchLen -= ML_MASK;
			WLZ_WRITE_ExtraLength(destPtr, matchLen);
        } else
            *token += (Uint8)(matchLen);

				anchor = srcPtr;
		matchLen = 0;
    }

_last_literals:
    /* Encode Last Literals */
    litLen = (int)(srcEnd - anchor);
        
#ifdef WLZ_DEBUG
	fprintf(fptr, "srcIdx=%d, destIdx=%d,  litLen=%d\n",
		(int)(anchor - (const Uint8*)source), (int)(destPtr - (Uint8*)destiny), litLen);
#endif

    if (litLen >= RUN_MASK) {
        extraLitLen = litLen - RUN_MASK;
        *destPtr++ = RUN_MASK << ML_BITS;
		WLZ_WRITE_ExtraLength(destPtr, extraLitLen);
    } else {
        *destPtr++ = (Uint8)(litLen <<ML_BITS );
    }
	memcpy(destPtr, anchor, litLen);                      /* exact: the input may end right here */
    destPtr += litLen;
	*destPtr++ = 0;     // append zero offset

	
   
#ifdef WLZ_DEBUG
    fprintf(fptr, "Compressed %i bytes into %i bytes\n", srcSize, (Uint32)(((char*)destPtr) - destiny));
	for (matchLen = MIN_MATCH_LEN; matchLen <= ML_MASK + MIN_MATCH_LEN; matchLen++)
		fprintf(fptr, "matchLen=%d:  %d\n", matchLen, matchStat[matchLen - MIN_MATCH_LEN]);
	fclose(fptr);
#endif
    return (Uint32)(((char*)destPtr) - destiny);
}


unsigned WLZ_Compress(WLZ_State_Str *wlzStr, const char* source, char* destiny, unsigned srcSize, unsigned destCapSize)
{
	
	unsigned destSizeBound = srcSize + WLZ_COMP_BOUND + 8;
	if (destCapSize < destSizeBound ) {
		fprintf(stderr, "Insufficient space for the worst-case compression, aborted\n");
		return 0;
	}

	WLZ_Init_State(wlzStr);
    return WLZ_Compress_Kernel(wlzStr, source, destiny, srcSize, 0);
}

unsigned WLZ_Compress_Fast(WLZ_State_Str *wlzStr, const char* source, char* destiny, unsigned srcSize, unsigned destCapSize, int acceleration)
{
	if (destCapSize < srcSize + WLZ_COMP_BOUND + 8) {
		printf("Insufficient space for the worst-case compression, aborted\n");
		return 0;
	}
	
	WLZ_Init_State(wlzStr);
	return WLZ_Compress_Kernel(wlzStr, source, destiny, srcSize, acceleration);
}


/* hidden WLZ_DEBUG function */
/* strangely enough, gcc generates faster code when this function is uncommented, even if unused */
unsigned WLZ_Compress_wDict(const char *dictionary, unsigned dictSize, const char* source, char* destiny, unsigned srcSize, unsigned destCapSize, int acceleration)
{
	if (destCapSize < srcSize + WLZ_COMP_BOUND + 8) {
		printf("Insufficient space for the worst-case compression, aborted\n");
		return 0;
	}
	unsigned result;
	WLZ_State_Str* const wlzStr = WLZ_New_State();       /* a local struct had no table: Init_State wrote through a wild pointer */
	if (wlzStr == NULL) return 0;

	WLZ_Load_Dictionary(wlzStr, dictionary, dictSize);

	result = WLZ_Compress_Kernel(wlzStr, source, destiny, srcSize, acceleration);

	WLZ_Free_State(wlzStr);
	return result;
}

unsigned WLZ_Compress_wDictStr(WLZ_State_Str dictStr, const char* source, char* destiny, unsigned srcSize, unsigned destCapSize, int acceleration)
{
	if (destCapSize < srcSize + WLZ_COMP_BOUND + 8) {
		printf("Insufficient space for the worst-case compression, aborted\n");
		return 0;
	}

	unsigned result;
	WLZ_State_Str* const wlzStr = WLZ_New_State();
	if (wlzStr == NULL) return 0;

	WLZ_Attach_Dictionary(wlzStr, &dictStr);

	result = WLZ_Compress_Kernel(wlzStr, source, destiny, srcSize, acceleration);

	WLZ_Free_State(wlzStr);
	return result;
}

/*-******************************
*  Streaming functions
********************************/

#ifndef _MSC_VER  /* for some reason, Visual fails the aligment test on 32-bit x86 :
                     it reports an aligment of 8-bytes,
                     while actually aligning WLZ_State_Str on 4 bytes. */
static int WLZ_State_Str_alignment(void)
{
    struct { char c; WLZ_State_Str t; } t_a;
    return sizeof(t_a) - sizeof(t_a.t);
}
#endif

WLZ_State_Str *WLZ_New_State()
{
	WLZ_State_Str* const wlzStr = (WLZ_State_Str*)calloc(1, sizeof(WLZ_State_Str));
	if (wlzStr == NULL) return NULL;
	wlzStr->hash2Table = (int *)calloc((1 << WLZ_HASH2BITS), sizeof(int));
	if (wlzStr->hash2Table == NULL) { free(wlzStr); return NULL; }
	return wlzStr;
}

void WLZ_Init_State(WLZ_State_Str *wlzStr)
{
	memset(wlzStr->hash1Table, 0, (1 << WLZ_HASH1BITS) * sizeof(int));
	memset(wlzStr->hash2Table, 0, (1 << WLZ_HASH2BITS) * sizeof(int));

	wlzStr->dictSize = 0;
	wlzStr->dictEnd = NULL;
}

void WLZ_Free_State (WLZ_State_Str *wlzStr)
{
	if (wlzStr == NULL) return;
	free(wlzStr->hash2Table);                             /* the dictionary is the caller's: only referenced */
	free(wlzStr);
}



unsigned WLZ_Load_Dictionary (WLZ_State_Str * wlzStr, const char* dictionary, unsigned dictSize)
{
	const Uint8* dictPtr;
	Uint32 hashV[2];
	int  dictIdx;
	wlzStr->dictSize = dictSize;
	wlzStr->dictEnd = (const Uint8*)dictionary + dictSize;
	int hashUnit = 8;     // max of reg_t and max-hash

	memset(wlzStr->hash1Table, 0, (1 << WLZ_HASH1BITS) * sizeof(int));
	if (wlzStr->hash2Table == NULL) wlzStr->hash2Table = (int *)calloc((1 << WLZ_HASH2BITS), sizeof(int));
	else memset(wlzStr->hash2Table, 0, (1 << WLZ_HASH2BITS) * sizeof(int));

   
    if (dictSize < hashUnit) {
        return 0;
    }

	for (dictPtr = wlzStr->dictEnd - MIN(dictSize, WLZ_MATCH2_WINDOW); dictPtr <= wlzStr->dictEnd - hashUnit; dictPtr++) {
		hashV[0] = WLZ_Hash1(dictPtr, WLZ_HASH1BITS);
		hashV[1] = WLZ_Hash2(dictPtr, WLZ_HASH2BITS);
		dictIdx = (int)(dictPtr - wlzStr->dictEnd);
		wlzStr->hash1Table[ hashV[0] ] = dictIdx;
		wlzStr->hash2Table[ hashV[1] ] = dictIdx;
    }

    return dictSize;
}

void WLZ_Attach_Dictionary(WLZ_State_Str *workStr, const WLZ_State_Str *dictStr) 
{
	if (dictStr == NULL) return;

	memcpy(workStr->hash1Table, dictStr->hash1Table, (1 << WLZ_HASH1BITS) * sizeof(int));
	memcpy(workStr->hash2Table, dictStr->hash2Table, (1 << WLZ_HASH2BITS) * sizeof(int));
    
	workStr->dictSize = dictStr->dictSize;
	workStr->dictEnd = dictStr->dictEnd;
}

/*! WLZ_Save_Dictionary() :
 *  If previously compressed data block is not guaranteed to remain available at its memory location,
 *  save it into a safer place (char* safeBuffer).
 *  Note : you don't need to call WLZ_loadDict() afterwards,
 *         dictionary is immediately usable, you can therefore call WLZ_compress_fast_continue().
 *  Return : saved dictionary size in bytes (necessarily <= dictSize), or 0 if error.
 */
unsigned WLZ_Save_Dictionary(WLZ_State_Str* dictStr, WLZ_State_Str* workStr)
{
	int i;
	for (i = 0; !(i >>WLZ_HASH1BITS); i++)
		dictStr->hash1Table[i] = workStr->hash1Table[i] == 0 ? 0 : workStr->hash1Table[i] - workStr->dictSize;

	for (i = 0; !(i >>WLZ_HASH2BITS); i++)
		dictStr->hash2Table[i] = workStr->hash2Table[i] == 0 ? 0 : workStr->hash2Table[i] - workStr->dictSize;

	dictStr->dictEnd = workStr->dictEnd;
	dictStr->dictSize = workStr->dictSize;
	return dictStr->dictSize;
}












/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 ******************************************************************** Hash-Chain Compression Functions  ********************************************************************
 ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
static int Search_Level_Map[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 1000 };

typedef struct wlz_match {
	int len;                                        // LZ match length
	int off;                                        // LZ match offset/distance  
} WLZ_Match;

ForceInlineTemplate void WLZhc_Search_Hash1Table(WLZhc_State_Str* const wlzStr, const Uint8 *source, int currIdx, WLZ_Match *matchStr)
{
	int *hash1Table = wlzStr->hash1Table;
	Uint8 *matchPtr, *srcPtr;
	Uint32 hashV0, matchDist;
	int matchIdx;
	reg_t diffPattern;

	srcPtr = source + wlzStr->curr1Idx;
	while(wlzStr->curr1Idx < currIdx-1) {
		hashV0 = WLZ_Hash1(++srcPtr, WLZhc_HASH1BITS);
		hash1Table[hashV0] = ++wlzStr->curr1Idx;
	}

	matchStr->len = 0;
	srcPtr = source + currIdx;
	reg_t currPattern = MemReadARCH(srcPtr);
	hashV0 = WLZ_Hash1(srcPtr, WLZhc_HASH1BITS);
	matchDist = currIdx - hash1Table[hashV0];
	hash1Table[hashV0] = currIdx;
	if (matchDist < WLZ_MATCH1_WINDOW) {
		matchIdx = currIdx - matchDist;
		matchPtr = (wlzStr->dictSize && matchIdx < 0) ? wlzStr->dictEnd + matchIdx : srcPtr - matchDist;
		diffPattern = currPattern ^ MemReadARCH(matchPtr);
		matchStr->len = diffPattern == 0 ? REG_SIZE : N_ZeroBytes(diffPattern);
		matchStr->off = matchDist;
	}
}

ForceInlineTemplate void WLZhc_Search_HashChain(WLZhc_State_Str* const wlzStr, const Uint8 *source, int currIdx, const Uint8 *srcLastMatch, const Uint8 *dictLastMatch, WLZ_Match *matchStr, int chainSearchCnt)
{
	Uint16 *chain2Table = wlzStr->chain2Table;
	int *hash2Table = wlzStr->hash2Table;
	Uint8 *matchPtr, *srcPtr;
	Uint32 hashV1, diffIdx, matchDist;
	int matchLen, matchIdx;
	const Uint32 dictSize = wlzStr->dictSize;
	const Uint8* const dictEnd = wlzStr->dictEnd;

	srcPtr = source + wlzStr->currIdx;
	while(wlzStr->currIdx < currIdx) {
		wlzStr->currIdx++;
		hashV1 = WLZ_Hash2(++srcPtr, WLZhc_HASH2BITS);
		diffIdx = hash2Table[hashV1] != 0 ? wlzStr->currIdx - hash2Table[hashV1] : WLZ_MAX_DIST;
		hash2Table[hashV1] = wlzStr->currIdx;
		chain2Table[(Uint16)wlzStr->currIdx] = (Uint16) (MIN(WLZ_MAX_DIST, diffIdx));
	}
	srcPtr = source + currIdx;

	Uint32 currPattern = MemRead4(srcPtr);
	matchDist = chain2Table[(Uint16)currIdx];
	while (matchDist < WLZ_MAX_DIST && (int)matchDist < currIdx + (int)dictSize && chainSearchCnt) {
		matchIdx = currIdx - matchDist;
		matchPtr = (dictSize && matchIdx < 0) ? dictEnd + matchIdx : srcPtr - matchDist;
		if (currPattern == MemRead4(matchPtr)) {
			matchLen = 4 + WLZ_Match_Count(srcPtr + 4, matchPtr + 4, srcLastMatch, dictLastMatch);
			if (matchLen > matchStr->len) {
				matchStr->len = matchLen;
				matchStr->off = matchDist;
			}
		}
		chainSearchCnt--;
		matchDist += chain2Table[(Uint16)matchIdx];
	}
}

ForceInlineTemplate int WLZhc_Search_HashChain_2D(WLZhc_State_Str* const wlzStr, const Uint8 *source, int currIdx, int maxBack, int nextMatchLen, const Uint8 *srcLastMatch, const Uint8 *dictLastMatch, WLZ_Match *matchStr, int chainSearchCnt)
{
	Uint16 *chain2Table = wlzStr->chain2Table;
	int *hash2Table = wlzStr->hash2Table;
	Uint8 *matchPtr, *srcPtr;
	Uint32 hashV, diffIdx, matchDist;
	int matchLen, matchIdx;
	int back, score, optBack = maxBack;
	Uint8 backByte[MAX_HASH_LEN + 1];
	int backIdx, backTable[8] = { 0, 1, 0, 2,  0, 1, 0, 3 };
	int vSearchCnt;
	Uint32 currPattern;
	const Uint32 dictSize = wlzStr->dictSize;
	const Uint8* const dictEnd = wlzStr->dictEnd;

	srcPtr = source + wlzStr->currIdx;
	while (wlzStr->currIdx < currIdx) {
		wlzStr->currIdx++;
		hashV = WLZ_Hash2(++srcPtr, WLZhc_HASH2BITS);
		diffIdx = hash2Table[hashV] != 0 ? wlzStr->currIdx - hash2Table[hashV] : WLZ_MAX_DIST;
		hash2Table[hashV] = wlzStr->currIdx;
		chain2Table[(Uint16)wlzStr->currIdx] = (Uint16)( MIN(WLZ_MAX_DIST, diffIdx) );
	}
	srcPtr = source + currIdx;

	vSearchCnt = chainSearchCnt;
	currPattern = MemRead4(srcPtr);
	backByte[0] = *(srcPtr - 1);
	backByte[1] = *(srcPtr - 2);
	backByte[2] = *(srcPtr - 3);
	//backByte[3] = *(srcPtr - 4);
	matchDist = chain2Table[(Uint16)currIdx];
	while (matchDist < WLZ_MAX_DIST && (int)matchDist < currIdx + (int)dictSize && vSearchCnt) {
		matchIdx = currIdx - matchDist;
		matchPtr = (dictSize && matchIdx < 0) ? dictEnd + matchIdx : srcPtr - matchDist;
		if (currPattern == MemRead4(matchPtr)) {
			matchLen = 4+ WLZ_Match_Count(srcPtr + 4, matchPtr + 4, srcLastMatch, dictLastMatch);

			//backIdx = (backByte[0] == *(matchPtr - 1));
			backIdx = (matchIdx >= 2 || (matchIdx < 0 && matchIdx >= 2 - (int)dictSize)) ? (backByte[0] == *(matchPtr - 1)) ^ ((backByte[1] == *(matchPtr - 2)) << 1) : 0;   /* never extend before the history start */
			//backIdx = (backByte[0] == *(matchPtr - 1)) ^ ((backByte[1] == *(matchPtr - 2)) << 1) ^ ((backByte[2] == *(matchPtr - 3)) << 2);
			back = backTable[ backIdx ];
			matchLen += back;
			score = matchLen - matchStr->len + (back-optBack) * nextMatchLen / maxBack ;
			if ( score>0 || (back<optBack && score==0) ) {
				matchStr->len = matchLen;
				matchStr->off = matchDist;
				optBack = back;
			}
			vSearchCnt--;
		}
		matchDist += chain2Table[(Uint16)matchIdx];
	}
	//if (maxBack - optBack > 0) return maxBack - optBack;

	vSearchCnt = 1 + chainSearchCnt/8;
	back = maxBack - 1;
	currIdx -= back;
	srcPtr -= back;
	currPattern = MemRead4(srcPtr);
	matchDist = chain2Table[(Uint16)currIdx];
	while (matchDist < WLZ_MAX_DIST && (int)matchDist < currIdx + (int)dictSize && vSearchCnt) {
		matchIdx = currIdx - matchDist;
		matchPtr = (dictSize && matchIdx < 0) ? dictEnd + matchIdx : srcPtr - matchDist;
		if (currPattern == MemRead4(matchPtr)) {
			matchLen = 4 + WLZ_Match_Count(srcPtr + 4, matchPtr + 4, srcLastMatch, dictLastMatch);
			score = matchLen - matchStr->len + (back - optBack) * nextMatchLen / maxBack;
			if (score >=0) {
				matchStr->len = matchLen;
				matchStr->off = matchDist;
				optBack = back; 
			}
			vSearchCnt--;
		}
		matchDist += chain2Table[(Uint16)matchIdx];
	}
	return maxBack - optBack;
}


 /** forced inline, to ensure branches are decided at compilation time **/
ForceInlineTemplate Uint32 WLZhc_Compress_Kernel(
	WLZhc_State_Str* const wlzStr,
	const char* const source,
	char* const destiny,
	const int srcSize,
	int validSearchLimit)
{
	const Uint8* srcPtr = (const Uint8*)source;
	const Uint8* anchor = (const Uint8*)source;
	const Uint8* const srcEnd = (const Uint8*)source + srcSize;
	const Uint8* const srcLastMatch = srcEnd - REG_SIZE * 2;
	Uint8* destPtr = (Uint8*)destiny;
	WLZ_Match matchStr, nextMatchStr = { 0, 0 };
	int lazyForward;
	Uint32 litLen, extraLitLen;
	const Uint32 dictSize = wlzStr->dictSize;
	const Uint8* const dictLastMatch = dictSize ? wlzStr->dictEnd - REG_SIZE * 2 : NULL;

	Uint8* token;

#ifdef WLZ_DEBUG 
	FILE *fptr = fopen("WLZhc_Compress_Index.txt", "w");
	fprintf(fptr, "WLZ_Compress_Kernel: srcSize=%i\n", srcSize);
	int matchStat[ML_MASK + 1] = { 0 };
#endif

	/* If init conditions are not met, we don't have to mark stream
	 * as having dirty context, since no action was taken yet */
	if ((Uint32)srcSize > (Uint32)WLZ_MAX_INPUT_SIZE) return 0;           /* Unsupported srcSize, too large (or negative) */
	destPtr = WLZ_Write_Size(destPtr, (Uint32)srcSize);
	Uint8* const payload = destPtr;                         /* the guard counts the payload only */

	if (srcSize <= MAX_HASH_LEN) goto _last_literals;        /* Input too small, no compression (all literals) */

	int srcIdx = 1;
	int nextMatchDone = 0;
	srcPtr++;	
	while (1) {

		while (1) {
			/*if (srcIdx == 11184) {
				srcIdx += 0;
			}*/

			if (unlikely(srcPtr >= srcLastMatch)) goto _last_literals;
			
			if (nextMatchDone) {
				matchStr = nextMatchStr;
				nextMatchDone = 0;
			}
			else {
				matchStr.len = 0;
				WLZhc_Search_HashChain(wlzStr, source, srcIdx, srcLastMatch, dictLastMatch, &matchStr, validSearchLimit);
			}
			if (matchStr.len >= MAX_HASH_LEN) break;

			WLZhc_Search_Hash1Table(wlzStr, source, srcIdx, &matchStr);
			if (matchStr.len >= MIN_MATCH_LEN) break;

			srcPtr ++;
			srcIdx ++;
		}
		if (likely(srcPtr + matchStr.len < srcLastMatch && srcPtr + MAX_HASH_LEN < srcLastMatch)) {
			
			if ( matchStr.off==1 ) {        // cut short repetative hash chain
				wlzStr->currIdx = max(wlzStr->currIdx, srcIdx + matchStr.len - MAX_HASH_LEN);
				wlzStr->curr1Idx = max(wlzStr->curr1Idx, srcIdx + matchStr.len - MAX_HASH_LEN);
			}

			nextMatchStr.len = 2;  /*nextMatchStr.off = WLZ_MAX_DIST;*/
			WLZhc_Search_HashChain(wlzStr, source, srcIdx + matchStr.len, srcLastMatch, dictLastMatch, &nextMatchStr, validSearchLimit);

			lazyForward = WLZhc_Search_HashChain_2D(wlzStr, source, srcIdx + 3, 3, nextMatchStr.len, srcLastMatch, dictLastMatch, &matchStr, 1+validSearchLimit/2 );
			
			
			nextMatchDone = (0==lazyForward);
			srcPtr += lazyForward;
						srcIdx += lazyForward;
		}

		litLen = (Uint32)(srcPtr - anchor);
		if (!WLZ_SEQ_PAYS(destPtr - payload, WLZ_Seq_Size(litLen, matchStr.len), srcPtr + matchStr.len - (const Uint8*)source)) {
			srcPtr++;                                           /* costs more than it saves: a literal, and search on */
			srcIdx++;
			nextMatchDone = 0;
			continue;
		}

		/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~   Encode Literal Run ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

		token = destPtr++;
		if (litLen >= RUN_MASK) {
			extraLitLen = litLen - RUN_MASK;
			*token = (RUN_MASK << ML_BITS);
			WLZ_WRITE_ExtraLength(destPtr, extraLitLen);
			MemWildCopy(destPtr + 16, anchor + 16, destPtr + litLen);
		}
		else *token = (Uint8)(litLen << ML_BITS);

		memcpy(destPtr, anchor, 16);
		destPtr += litLen;

#ifdef WLZ_DEBUG
		matchStat[min(ML_MASK, matchStr.len - MIN_MATCH_LEN)]++;
		fprintf(fptr, "srcIdx=%d, destIdx=%d,  litLen=%d,  matchLen=%d, matchOffset=%d\n",
			(int)(anchor - (const Uint8*)source), (int)(destPtr - (Uint8*)destiny), litLen, matchStr.len, matchStr.off);
		fflush(fptr);
#endif

		/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~   Encode Match Pair  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
		srcIdx += matchStr.len;
		srcPtr += matchStr.len;

		matchStr.len -= MIN_MATCH_LEN;
		MemWriteLE2(destPtr, (Uint16)matchStr.off);
		destPtr += matchStr.len < MAX_HASH_LEN - MIN_MATCH_LEN ? 1 : 2;

		if (matchStr.len >= ML_MASK) {
			*token += ML_MASK;
			matchStr.len -= ML_MASK;
			WLZ_WRITE_ExtraLength(destPtr, matchStr.len);
		}
		else
			*token += (Uint8)(matchStr.len);

				anchor = srcPtr;
	}

_last_literals:
	/* Encode Last Literals */
	litLen = (int)(srcEnd - anchor);

#ifdef WLZ_DEBUG
	fprintf(fptr, "srcIdx=%d, destIdx=%d,  litLen=%d\n",
		(int)(anchor - (const Uint8*)source), (int)(destPtr - (Uint8*)destiny), litLen);
#endif

	if (litLen >= RUN_MASK) {
		extraLitLen = litLen - RUN_MASK;
		*destPtr++ = RUN_MASK << ML_BITS;
		WLZ_WRITE_ExtraLength(destPtr, extraLitLen);
	}
	else {
		*destPtr++ = (Uint8)(litLen << ML_BITS);
	}
	memcpy(destPtr, anchor, litLen);                      /* exact: the input may end right here */
	destPtr += litLen;
	*destPtr++ = 0;     // append zero offset

	

#ifdef WLZ_DEBUG
	fprintf(fptr, "Compressed %i bytes into %i bytes\n", srcSize, (Uint32)(((char*)destPtr) - destiny));
	for (matchStr.len = MIN_MATCH_LEN; matchStr.len <= ML_MASK + MIN_MATCH_LEN; matchStr.len++)
		fprintf(fptr, "matchLen=%d:  %d\n", matchStr.len, matchStat[matchStr.len - MIN_MATCH_LEN]);
	fclose(fptr);
#endif
	return (Uint32)(((char*)destPtr) - destiny);
}



/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  Optimal parsing (levels 8-12): the parse of least compressed size, a shortest path over byte-exact prices.
 *  WLZ4 costs are whole bytes: a sequence is a token, its literals (plus extension bytes from 15 on), an offset of
 *  one byte for lengths 3-4 (window 256) or two bytes for longer ones (window 64K), and match-length extension bytes.
 *  At each position two candidates cover every codable match: the longest match within 256 (lengths 3-4 with a
 *  one-byte offset) and the longest within 64K (lengths 5 and up). As in LZ4HC's optimal parser, each position keeps
 *  its cheapest path, with the length of its pending literal run.
 *~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
#define WLZ_OPT_NUM         (1 << 12)
#define WLZ_OPT_TRAILING    3
#define WLZ_OPT_INF         (1 << 30)

typedef struct {
	int price;                                      /* bytes to code the input up to here */
	int mlen;                                       /* match ending here, or 1: a literal */
	int off;
	int litlen;                                     /* literals ending here (after the last match) */
} WLZ_Opt_Node;

typedef struct {
	int nbSearches;                                 /* chain steps in the 64K window */
	int nbShort;                                    /* chain steps in the 256 window */
	int sufficientLen;                              /* a match this long is taken at once */
	int fullUpdate;                                 /* search every position whose price may still improve */
} WLZ_Opt_Params;

#define WLZhc_OPT_LEVEL_MIN 8
#define WLZhc_LEVEL_MAX     12

/* from level 8 on, optimal parsing is both smaller and faster than lazy parsing with deeper chains */
static const WLZ_Opt_Params WLZ_Opt_Level[WLZhc_LEVEL_MAX - WLZhc_OPT_LEVEL_MIN + 1] = {
	{    16,  8,   32, 1 },                         /*  8 */
	{    32,  8,   64, 1 },                         /*  9 */
	{    64, 16,   64, 1 },                         /* 10: smaller than LZ4HC level 12 */
	{   256, 32,  128, 1 },                         /* 11 */
	{  4096, 64, WLZ_OPT_NUM, 1 },                  /* 12 */
};

/* bytes of a length extension of value v (a literal run or match-length code beyond 14) */
ForceInlineTemplate int WLZ_Ext_Bytes(int v)
{
	if (v < 252) return 1;
	v -= 252;
	return 2 + (v > 0xFF) + (v > 0xFFFF) + (v > 0xFFFFFF);
}

ForceInlineTemplate int WLZ_Lit_Price(int litLen)
{
	return litLen + (litLen >= (int)RUN_MASK ? WLZ_Ext_Bytes(litLen - (int)RUN_MASK) : 0);
}

/* a sequence: token, its literal run, the offset (one byte for lengths 3-4), the match-length extension */
ForceInlineTemplate int WLZ_Seq_Price(int litLen, int matchLen)
{
	const int code = matchLen - MIN_MATCH_LEN;
	return 1 + WLZ_Lit_Price(litLen) + (matchLen < MAX_HASH_LEN ? 1 : 2) + (code >= (int)ML_MASK ? WLZ_Ext_Bytes(code - (int)ML_MASK) : 0);
}

/* writes one sequence: the literals from anchor, then the match */
ForceInlineTemplate Uint8* WLZ_Encode_Sequence(Uint8* destPtr, const Uint8* anchor, Uint32 litLen, Uint32 matchLen, Uint32 matchOffset)
{
	Uint8* const token = destPtr++;
	if (litLen >= RUN_MASK) {
		Uint32 extraLitLen = litLen - RUN_MASK;
		*token = (RUN_MASK << ML_BITS);
		WLZ_WRITE_ExtraLength(destPtr, extraLitLen);
		MemWildCopy(destPtr + 16, anchor + 16, destPtr + litLen);
	}
	else *token = (Uint8)(litLen << ML_BITS);
	memcpy(destPtr, anchor, 16);
	destPtr += litLen;

	matchLen -= MIN_MATCH_LEN;
	MemWriteLE2(destPtr, (Uint16)matchOffset);
	destPtr += matchLen < MAX_HASH_LEN - MIN_MATCH_LEN ? 1 : 2;
	if (matchLen >= ML_MASK) {
		*token += ML_MASK;
		matchLen -= ML_MASK;
		WLZ_WRITE_ExtraLength(destPtr, matchLen);
	}
	else *token += (Uint8)matchLen;
	return destPtr;
}

/* inserts the positions up to target: the 5-byte hash chain over 64K, the 3-byte chain over 256 (chain1, by the low byte) */
ForceInlineTemplate void WLZ_Opt_Insert(WLZhc_State_Str* const s, const Uint8* const src, const int target, Uint8* const chain1)
{
	int idx = s->currIdx;
	while (idx < target) {
		idx++;
		const Uint32 h2 = WLZ_Hash2(src + idx, WLZhc_HASH2BITS);
		const Uint32 d2 = s->hash2Table[h2] ? (Uint32)(idx - s->hash2Table[h2]) : WLZ_MAX_DIST;
		s->hash2Table[h2] = idx;
		s->chain2Table[(Uint16)idx] = (Uint16)MIN(WLZ_MAX_DIST, d2);
		const Uint32 h1 = WLZ_Hash1(src + idx, WLZhc_HASH1BITS);
		const Uint32 d1 = s->hash1Table[h1] ? (Uint32)(idx - s->hash1Table[h1]) : WLZ_MATCH1_WINDOW;
		s->hash1Table[h1] = idx;
		chain1[(Uint8)idx] = (Uint8)(d1 < WLZ_MATCH1_WINDOW ? d1 : 0);
	}
	s->currIdx = idx;
}

/* the longest match at idx within 256 (any length from 3), and the longest within 64K (length 5 and up) */
ForceInlineTemplate void WLZ_Opt_Find(WLZhc_State_Str* const s, const Uint8* const src, const int idx, const Uint8* const srcLastMatch,
	const Uint8* const chain1, const WLZ_Opt_Params* const par, WLZ_Match* const shortM, WLZ_Match* const longM)
{
	const Uint8* const ip = src + idx;
	const Uint32 pattern4 = MemRead4(ip);
	shortM->len = 0; shortM->off = 0;
	longM->len = 0; longM->off = 0;

	/* 256 window, by the 3-byte chain */
	{   Uint32 dist = chain1[(Uint8)idx];
		int n = par->nbShort;
		while (dist && dist < WLZ_MATCH1_WINDOW && n--) {
			const Uint8* const mp = ip - dist;
			const reg_t diff = MemReadARCH(ip) ^ MemReadARCH(mp);
			int len = diff ? (int)N_ZeroBytes(diff) : REG_SIZE + WLZ_Match_Count((Uint8*)ip + REG_SIZE, (Uint8*)mp + REG_SIZE, srcLastMatch, NULL);
			if (len > shortM->len) {
				shortM->len = len; shortM->off = (int)dist;
				if (len >= MAX_HASH_LEN) break;             /* longer lengths take two offset bytes: the 64K search covers them */
			}
			const Uint32 step = chain1[(Uint8)(idx - dist)];
			if (!step) break;
			dist += step;
		}
	}
	/* 64K window, by the 5-byte chain */
	{   Uint32 dist = s->chain2Table[(Uint16)idx];
		int n = par->nbSearches;
		int bestLen = MAX_HASH_LEN - 1;
		while (dist < WLZ_MAX_DIST && n--) {
			const Uint8* const mp = ip - dist;
			if (MemRead4(mp) == pattern4 && mp[bestLen] == ip[bestLen]) {
				const int len = 4 + WLZ_Match_Count((Uint8*)ip + 4, (Uint8*)mp + 4, srcLastMatch, NULL);
				if (len > bestLen) {
					bestLen = len; longM->len = len; longM->off = (int)dist;
					if (len >= par->sufficientLen) break;
				}
			}
			dist += s->chain2Table[(Uint16)(idx - dist)];
		}
	}
	if (shortM->len >= MAX_HASH_LEN && shortM->len > longM->len) *longM = *shortM;   /* the 3-byte chain found the longer one */
	if (shortM->len > MAX_HASH_LEN - 1) shortM->len = MAX_HASH_LEN - 1;
	if (shortM->len < MIN_MATCH_LEN && longM->len && longM->off < WLZ_MATCH1_WINDOW) {   /* a near long match: its short prefixes */
		shortM->len = MAX_HASH_LEN - 1; shortM->off = longM->off;
	}
	if (shortM->len < MIN_MATCH_LEN) shortM->len = 0;
}

static Uint32 WLZhc_Compress_Optimal(WLZhc_State_Str* const wlzStr, const char* const source, char* const destiny, const int srcSize, const WLZ_Opt_Params* const par)
{
	const Uint8* const src = (const Uint8*)source;
	const Uint8* ip = src + 1;
	const Uint8* anchor = src;
	const Uint8* const srcEnd = src + srcSize;
	const Uint8* const srcLastMatch = srcEnd - REG_SIZE * 2;
	Uint8* destPtr = (Uint8*)destiny;
	Uint8 chain1[WLZ_MATCH1_WINDOW] = { 0 };
	WLZ_Opt_Node* const opt = (WLZ_Opt_Node*)malloc((WLZ_OPT_NUM + WLZ_OPT_TRAILING + 64) * sizeof(WLZ_Opt_Node));
	WLZ_Match shortM, longM;
	const int sufficientLen = min(par->sufficientLen, WLZ_OPT_NUM - 1);
	Uint32 litLen, extraLitLen;

			if ((Uint32)srcSize > (Uint32)WLZ_MAX_INPUT_SIZE || opt == NULL) { free(opt); return 0; }
	destPtr = WLZ_Write_Size(destPtr, (Uint32)srcSize);
	Uint8* const payload = destPtr;                         /* the guard counts the payload only */
	if (srcSize <= MAX_HASH_LEN * 4) goto _last_literals;

	while (ip < srcLastMatch) {
		const int llen = (int)(ip - anchor);
		int cur, lastPos, bestLen, bestOff, known;           /* known: positions up to here are initialized */

		WLZ_Opt_Insert(wlzStr, src, (int)(ip - src), chain1);
		WLZ_Opt_Find(wlzStr, src, (int)(ip - src), srcLastMatch, chain1, par, &shortM, &longM);
		if (!shortM.len && !longM.len) { ip++; continue; }

				if (longM.len >= sufficientLen) {              /* good enough: coded at once */
			if (WLZ_SEQ_PAYS(destPtr - payload, WLZ_Seq_Size((Uint32)llen, (Uint32)longM.len), ip + longM.len - src)) {
				destPtr = WLZ_Encode_Sequence(destPtr, anchor, (Uint32)llen, (Uint32)longM.len, (Uint32)longM.off);
				anchor = ip + longM.len;
			}
			ip += longM.len;
			continue;
		}

		/* the first position: literals, then its matches */
		for (int r = 0; r < MIN_MATCH_LEN; r++) {
			opt[r].price = WLZ_Lit_Price(llen + r); opt[r].mlen = 1; opt[r].off = 0; opt[r].litlen = llen + r;
		}
		lastPos = max(shortM.len, longM.len);
		for (int r = MIN_MATCH_LEN; r <= lastPos + WLZ_OPT_TRAILING; r++) opt[r].price = WLZ_OPT_INF;
		for (int ml = MIN_MATCH_LEN; ml <= shortM.len; ml++) {
			opt[ml].price = WLZ_Seq_Price(llen, ml); opt[ml].mlen = ml; opt[ml].off = shortM.off; opt[ml].litlen = llen;
		}
		for (int ml = MAX_HASH_LEN; ml <= longM.len; ml++) {
			opt[ml].price = WLZ_Seq_Price(llen, ml); opt[ml].mlen = ml; opt[ml].off = longM.off; opt[ml].litlen = llen;
		}
		for (int a = 1; a <= WLZ_OPT_TRAILING; a++) {
			opt[lastPos + a].price = opt[lastPos].price + WLZ_Lit_Price(a); opt[lastPos + a].mlen = 1; opt[lastPos + a].off = 0; opt[lastPos + a].litlen = a;
		}
		known = lastPos + WLZ_OPT_TRAILING;

		/* the following positions */
		for (cur = 1; cur < lastPos; cur++) {
			const Uint8* const curPtr = ip + cur;
			if (curPtr >= srcLastMatch) break;
			if (opt[cur].price >= WLZ_OPT_INF) continue;      /* not reachable (no match of that length) */
			if (par->fullUpdate) {
				/* nothing to gain where the next position is no dearer, unless a short match could still pay off */
				if (opt[cur + 1].price <= opt[cur].price && opt[cur + MIN_MATCH_LEN].price < opt[cur].price + 2) continue;
			}
			else if (opt[cur + 1].price <= opt[cur].price) continue;

			WLZ_Opt_Insert(wlzStr, src, (int)(curPtr - src), chain1);
			WLZ_Opt_Find(wlzStr, src, (int)(curPtr - src), srcLastMatch, chain1, par, &shortM, &longM);
			if (!shortM.len && !longM.len) continue;

			if (longM.len >= sufficientLen || cur + longM.len >= WLZ_OPT_NUM) {    /* coded at once */
				bestLen = longM.len; bestOff = longM.off;
				lastPos = cur + 1;
				goto _encode;
			}

			/* literals after cur */
			{   const int baseLit = opt[cur].litlen;
				for (int l = 1; l < MIN_MATCH_LEN; l++) {
					const int price = opt[cur].price - WLZ_Lit_Price(baseLit) + WLZ_Lit_Price(baseLit + l);
					if (price < opt[cur + l].price) {
						opt[cur + l].price = price; opt[cur + l].mlen = 1; opt[cur + l].off = 0; opt[cur + l].litlen = baseLit + l;
					}
				}
			}
			/* matches from cur */
			{   const int ll = opt[cur].mlen == 1 ? opt[cur].litlen : 0;
				const int base = opt[cur].mlen == 1 ? (cur > ll ? opt[cur - ll].price : 0) : opt[cur].price;
				const int maxLen = max(shortM.len, longM.len);
				for (int ml = MIN_MATCH_LEN; ml <= maxLen; ml++) {
					const int off = ml < MAX_HASH_LEN ? shortM.off : longM.off;
					if (ml < MAX_HASH_LEN ? ml > shortM.len : ml > longM.len) continue;
					const int pos = cur + ml;
					const int price = base + WLZ_Seq_Price(ll, ml);
					while (known < pos) opt[++known].price = WLZ_OPT_INF;     /* fresh positions */
					if (price <= opt[pos].price) {
						if (ml == maxLen && lastPos < pos) lastPos = pos;
						opt[pos].price = price; opt[pos].mlen = ml; opt[pos].off = off; opt[pos].litlen = ll;
					}
				}
			}
			for (int a = 1; a <= WLZ_OPT_TRAILING; a++) {
				opt[lastPos + a].price = opt[lastPos].price + WLZ_Lit_Price(a); opt[lastPos + a].mlen = 1; opt[lastPos + a].off = 0; opt[lastPos + a].litlen = a;
			}
			if (known < lastPos + WLZ_OPT_TRAILING) known = lastPos + WLZ_OPT_TRAILING;
		}

		bestLen = opt[lastPos].mlen;
		bestOff = opt[lastPos].off;
		cur = lastPos - bestLen;

	_encode:    /* back-trace from the last match, then code the sequences in order */
		{   int candidate = cur, selLen = bestLen, selOff = bestOff;
			while (1) {
				const int nextLen = opt[candidate].mlen, nextOff = opt[candidate].off;
				opt[candidate].mlen = selLen; opt[candidate].off = selOff;
				selLen = nextLen; selOff = nextOff;
				if (nextLen > candidate) break;
				candidate -= nextLen;
			}
		}
		{   int r = 0;
			while (r < lastPos) {
				const int ml = opt[r].mlen, off = opt[r].off;
				if (ml == 1) { ip++; r++; continue; }
								r += ml;
				if (WLZ_SEQ_PAYS(destPtr - payload, WLZ_Seq_Size((Uint32)(ip - anchor), (Uint32)ml), ip + ml - src)) {
					destPtr = WLZ_Encode_Sequence(destPtr, anchor, (Uint32)(ip - anchor), (Uint32)ml, (Uint32)off);
					anchor = ip + ml;                   /* else it costs more than it saves: its bytes stay literals */
				}
				ip += ml;
			}
		}
	}

_last_literals:
	free(opt);
	litLen = (Uint32)(srcEnd - anchor);
	if (litLen >= RUN_MASK) {
		extraLitLen = litLen - RUN_MASK;
		*destPtr++ = RUN_MASK << ML_BITS;
		WLZ_WRITE_ExtraLength(destPtr, extraLitLen);
	}
	else *destPtr++ = (Uint8)(litLen << ML_BITS);
	memcpy(destPtr, anchor, litLen);                    /* exact: the input may end right here */
	destPtr += litLen;
	*destPtr++ = 0;                                     /* a zero offset ends the block */
	
	return (Uint32)(destPtr - (Uint8*)destiny);
}

WLZhc_State_Str *WLZhc_New_State()
{
	WLZhc_State_Str* const wlzStr = (WLZhc_State_Str*)calloc(1, sizeof(WLZhc_State_Str));
	if (wlzStr == NULL) return NULL;
	wlzStr->hash2Table = (int *)malloc((1 << WLZhc_HASH2BITS) * sizeof(int));
	wlzStr->chain2Table = (Uint16 *)malloc(WLZ_MATCH2_WINDOW * sizeof(short));
	if (wlzStr->hash2Table == NULL || wlzStr->chain2Table == NULL) { WLZhc_Free_State(wlzStr); return NULL; }
	return wlzStr;
}
void WLZhc_Init_State(WLZhc_State_Str *wlzStr)
{

	wlzStr->currIdx = wlzStr->curr1Idx = 1;
	memset(wlzStr->hash1Table, 0, (1 << WLZhc_HASH1BITS) * sizeof(int));

	memset(wlzStr->hash2Table, 0, (1 << WLZhc_HASH2BITS) * sizeof(int));
	memset(wlzStr->chain2Table, 0xFF, WLZ_MATCH2_WINDOW * sizeof(short));

	wlzStr->dictSize = 0;
	wlzStr->dictEnd = NULL;
}

void WLZhc_Free_State(WLZhc_State_Str *wlzStr)
{
	if (wlzStr == NULL) return;
	free(wlzStr->hash2Table);
	free(wlzStr->chain2Table);                            /* the dictionary is the caller's: only referenced */
	free(wlzStr);
}

unsigned WLZhc_Load_Dictionary(WLZhc_State_Str * wlzStr, const char* dictionary, unsigned dictSize)
{
	const Uint8* dictPtr;
	Uint32 hashV1, hashV2;
	int  dictIdx;

	WLZhc_Init_State(wlzStr);
	wlzStr->dictSize = dictSize;
	wlzStr->dictEnd = (const Uint8*)dictionary + dictSize;
	unsigned hashUnit = 8;     // max of reg_t and max-hash

	if (dictSize < hashUnit) {
		return 0;
	}

	for (dictPtr = wlzStr->dictEnd - MIN(dictSize, WLZ_MATCH2_WINDOW); dictPtr <= wlzStr->dictEnd - hashUnit; dictPtr++) {
		hashV1 = WLZ_Hash1(dictPtr, WLZhc_HASH1BITS);
		hashV2 = WLZ_Hash2(dictPtr, WLZhc_HASH2BITS);
		dictIdx = (int)(dictPtr - wlzStr->dictEnd);
		wlzStr->chain2Table[(Uint16)(dictIdx + WLZ_MATCH2_WINDOW )] = (Uint16)(dictIdx - wlzStr->hash2Table[hashV2]);
		wlzStr->hash1Table[ hashV1 ] = dictIdx;
		wlzStr->hash2Table[ hashV2 ] = dictIdx;
	}

	return dictSize;
}

void WLZhc_Attach_Dictionary(WLZhc_State_Str *workStr, const WLZhc_State_Str *dictStr)
{
	if (dictStr == NULL) return;

	memcpy(workStr->hash1Table, dictStr->hash1Table, (1 << WLZhc_HASH1BITS) * sizeof(int));
	memcpy(workStr->hash2Table, dictStr->hash2Table, (1 << WLZhc_HASH2BITS) * sizeof(int));
	memcpy(workStr->chain2Table, dictStr->chain2Table, WLZ_MATCH2_WINDOW * sizeof(short));

	workStr->dictSize = dictStr->dictSize;
	workStr->dictEnd = dictStr->dictEnd;
}

int WLZhc_Save_Dictionary(WLZhc_State_Str* dictStr, WLZhc_State_Str* workStr)
{
	int i;
	for (i = 0; !(i >> WLZhc_HASH1BITS); i++)
		dictStr->hash1Table[i] = workStr->hash1Table[i] == 0 ? 0 : workStr->hash1Table[i] - workStr->dictSize;

	for (i = 0; !(i >> WLZhc_HASH2BITS); i++)
		dictStr->hash2Table[i] = workStr->hash2Table[i] == 0 ? 0 : workStr->hash2Table[i] - workStr->dictSize;
	memcpy(dictStr->chain2Table, workStr->chain2Table, WLZ_MATCH2_WINDOW * sizeof(short));

	dictStr->dictEnd = workStr->dictEnd;
	dictStr->dictSize = workStr->dictSize;
	return dictStr->dictSize;
}

unsigned WLZhc_Compress(WLZhc_State_Str *wlzStr, const char* source, char* destiny, unsigned srcSize, unsigned destCapSize, int level)
{

	unsigned destSizeBound = srcSize + WLZ_COMP_BOUND + 8;
	if (destCapSize < destSizeBound) {
		fprintf(stderr, "Insufficient space for the worst-case compression, aborted\n");
		return 0;
	}
	
	WLZhc_Init_State(wlzStr);

	/* levels 0-7: lazy parsing, 8-12: optimal parsing */
	if (level < 0) level = 0;
	if (level >= WLZhc_OPT_LEVEL_MIN) return WLZhc_Compress_Optimal(wlzStr, source, destiny, (int)srcSize, &WLZ_Opt_Level[min(level, WLZhc_LEVEL_MAX) - WLZhc_OPT_LEVEL_MIN]);
	return WLZhc_Compress_Kernel(wlzStr, source, destiny, srcSize, Search_Level_Map[level]);
}



/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 ************************************************************************  Decompression Functions  ************************************************************************
 ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

/* the decoded size, from the block header (srcSize: the compressed size, for a length check only) */
unsigned WLZ_Read_DecSize(const char *source, unsigned srcSize)
{
	if (srcSize < 2) return 0;
	const unsigned lo = MemReadLE2(source);
	if (!(lo >> 15)) return lo;
	if (srcSize < 4) return 0;
	return (lo & ((1 << 15) - 1)) | ((unsigned)MemReadLE2(source + 2) << 15);
}

/* bytes of the block header */
static unsigned WLZ_Size_Bytes(const char *source)
{
	return (MemReadLE2(source) >> 15) ? 4 : 2;
}


#define WLZ_READ_ExtraLength(srcPtr, extLen)   {                \
	extLen = *srcPtr++;                                         \
	if ( unlikely(extLen > 251) ) {                             \
		token = extLen - 251;                                   \
		extLen = (Uint32)(252 + (MemReadLE4(srcPtr) & ByteMask[token]));    \
        srcPtr += token;                                        \
    }                                                           \
}

 /*   This generic decompression function covers all use cases.
  *   Note that it is important for performance that this function really get inlined,
  *   in order to remove useless branches during compilation optimization.
  */
ForceInlineTemplate unsigned
WLZ_Decompress_Kernel(
                 const char* const src,
                 char* const dest,
                 const Uint8* const dictEnd,  
                 const unsigned dictSize)
{
	if (src == NULL) {
		printf("Err: Source is empty\n");
		return 0;
	}

	static const int inc3table[8] = { 0, -2, -3,  -2, -3,  1, 1, 1 };
	static const int dec8table[8] = { 0, 0, 0, -1, 0,  1, 2, 3 };

    const Uint8* srcPtr = (const Uint8*) src;
    Uint8* destPtr = (Uint8*) dest;
    const Uint8* match;
    register unsigned token;
	register unsigned litLen, matchLen, offset;
	static const unsigned offMask[2] = { 0xFF, 0xFFFF };

    while (1) {
		/*if (322 == (destPtr - (const Uint8*)dest)) {
			litLen += 0;
		}*/
		
        token = *srcPtr++;
        litLen = token >> ML_BITS;    /* literal length */
		matchLen = MIN_MATCH_LEN + (token & ML_MASK); /* match length */
		
		if ( litLen == RUN_MASK ) {
			WLZ_READ_ExtraLength(srcPtr, litLen);
			litLen += RUN_MASK;
			MemWildCopy(destPtr+16, srcPtr+16, destPtr + litLen);   
		}	

		MemCopy16(destPtr, srcPtr);
		srcPtr += litLen;
		destPtr += litLen;  
		token = (matchLen >= MAX_HASH_LEN);
		offset = MemReadLE2(srcPtr) & offMask[token];		
		srcPtr += token + 1;   /* number of offset bytes */

		if (dictSize && (ptrdiff_t)(destPtr - (const Uint8*)dest) < (ptrdiff_t)offset)
			match = dictEnd + ((ptrdiff_t)(destPtr - (const Uint8*)dest) - (ptrdiff_t)offset);
		else 
			match = destPtr - offset;
                
		
		if (likely( offset >> 3 )) {
			memcpy(destPtr, match, 8);
			match += 8;
		}
		else {
			if (unlikely(!offset)) {
				return (unsigned)(destPtr - (const Uint8*)dest);
			}
			
			*destPtr++ = *match++;
			*destPtr++ = *match++;
			*destPtr++ = *match++;
			*destPtr++ = *match;
			match += inc3table[offset];
			memcpy(destPtr, match, 4);
			destPtr -= 4;
			match -= dec8table[offset];
		}	
	
		destPtr[8] = *match++;
		memcpy(destPtr + 9, match, 8);
		if (unlikely(matchLen == ML_MASK + MIN_MATCH_LEN)) {
			memcpy(destPtr + 17, match + 8, 8);
			WLZ_READ_ExtraLength(srcPtr, matchLen);
			matchLen += ML_MASK + MIN_MATCH_LEN;
			MemWildCopy_Overlap(destPtr + 25, match + 16, destPtr + matchLen);
		}
		destPtr += matchLen;
    }

    /* end of decoding */
    return (unsigned) (destPtr- (const Uint8*)dest);     /* Nb of output bytes decoded */
}



/*===== Instantiate the API decoding functions. =====*/

FORCE_O2_GCC_PPC64LE
unsigned WLZ_Decompress(const char* source, char* destiny, unsigned compressedSize, unsigned decCapSize)
{
	if (decCapSize < WLZ_Read_DecSize(source, compressedSize) + WLZ_MEM_OVERHEAD) {
		printf("Insufficient decompression space.\n");
		return 0;
	}

    	if (compressedSize < 4) return 0;                     /* header, token and the ending zero offset at least */
	return WLZ_Decompress_Kernel(source + WLZ_Size_Bytes(source), destiny, NULL, 0);
}

FORCE_O2_GCC_PPC64LE
unsigned WLZ_Decompress_wDict(const char* source, char* destiny, unsigned compressedSize, unsigned decCapSize,
                                     const char* dictionary, unsigned dictSize)
{
	if (decCapSize < WLZ_Read_DecSize(source, compressedSize) + WLZ_MEM_OVERHEAD) {
		printf("Insufficient decompression space.\n");
		return 0; 
	}

	const Uint8* const dictEnd = (const Uint8*)dictionary + dictSize;

		if (compressedSize < 4) return 0;
	return WLZ_Decompress_Kernel(source + WLZ_Size_Bytes(source), destiny, dictEnd, dictSize);
}


#endif   /* WLZ_COMMONDEFS_ONLY */
