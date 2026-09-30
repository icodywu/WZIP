/*
 * WZIP - Huffman decoding of literal blocks
 * Copyright (c) 2018-present, Yingquan (Cody) Wu.
 * SPDX-License-Identifier: BSD-2-Clause (see LICENSE)
 */

/*MSB first packing of a multi-bit symbol. Herein we assume symBits is less than 32.
 sym: Symbol to be packed
 symBits: number of bits representing symbol
 The function packs MSB first so that Huffman Codes can retain their prefix property
*/

#include <string.h>     /* memcpy, memset */
#include <stdio.h>      /* fprintf (debug) */

#include "BitStream_Huffman.h"
#include "Memry.h"

/* We provide three different kind of Huffman decompression 
   X0: Decompressing one symbol a time. Fastest in building demapping table, while slow in decompressing a symbol.  
   X1: Decompressing one symbol a time. Fast in building demapping table, while fast in decompressing a symbol.     
   X2: Decompressing up to two symbols a time. Slow in building two-level demapping table. while fastest in decompressing up to two symbols. 
   Memory Size:    X0->1X;  X1->2X;   X2->4X
*/

void Build_Huffman_DecTableX0(const Uint32 hufCodeSize, const Uint32 maxHufCodeBits, Uint8* hufCodeBits, Uint8* hufCodeDemap)
{
	Uint32 i, nBits;
	Uint32 n,extLen;
	Uint32 nEffLits;
	Uint32 hufWtFreq[MAX_HufWeight + 1] = { 0 };   /* frequency of hufCodeBits  */
	Uint32 cumFreq[MAX_HufWeight + 2];             /* cumulative frequency count of Huffman widths */
	Huffman_DemapX1 sortHufCode[MAX_HufSize] = { 0 };
	Uint8* hufCodeDemapPtr;

	/* nEffLits = Fast_Sort_Width(hufCodeBits, hufCodeSize, maxHufCodeBits, sortHufCode, cumFreq); */
	for (i = 0; i < hufCodeSize; i++)
		hufWtFreq[hufCodeBits[i]]++;

	cumFreq[1] = 0;
	for (i = 1; i <= maxHufCodeBits; i++) {
		cumFreq[i + 1] = cumFreq[i] + hufWtFreq[i];
	}
	nEffLits = cumFreq[maxHufCodeBits + 1];
	cumFreq[0] = cumFreq[maxHufCodeBits + 1];       /* to place zero-freqency literals at the end */

	/* radix sorting literals in increasing order of litHufBits, while maintaining the original order for ones with equal litHufBits */
	for (i = 0; i < hufCodeSize; i++) {
		nBits = hufCodeBits[i];		
		sortHufCode[ cumFreq[nBits] ].lit = (Uint8)i;
		sortHufCode[ cumFreq[nBits]++ ].nbits = (Uint8)nBits;
	}
	cumFreq[0] = 0;                                 /* reset original meaning */

	switch (maxHufCodeBits) {
	case 1:
		hufCodeDemap[0] = sortHufCode[0].lit;
		hufCodeDemap[1] = sortHufCode[1].lit;
		return;
	case 2:
		if (nEffLits == 3) {
			hufCodeDemap[0] = sortHufCode[0].lit;
			hufCodeDemap[1] = sortHufCode[0].lit;
			hufCodeDemap[2] = sortHufCode[1].lit;
			hufCodeDemap[3] = sortHufCode[2].lit;
		}
		else {
			hufCodeDemap[0] = sortHufCode[0].lit;
			hufCodeDemap[1] = sortHufCode[1].lit;
			hufCodeDemap[2] = sortHufCode[2].lit;
			hufCodeDemap[3] = sortHufCode[3].lit;
		}
		return;
	default:
		;
	}

	extLen = 1<<(maxHufCodeBits - sortHufCode[0].nbits);
	memset(hufCodeDemap, sortHufCode[0].lit, extLen);
	hufCodeDemapPtr = hufCodeDemap + extLen;

	for (n = 1; n < cumFreq[maxHufCodeBits-3]; n++) {
		extLen = 1<< (maxHufCodeBits - sortHufCode[n].nbits);
		memset(hufCodeDemapPtr, sortHufCode[n].lit, extLen);
		hufCodeDemapPtr += extLen;
	}

	/* Fast demapping treatment for the case nbits == maxHufCodeBits-2, i.e., extBits==2 */
	for (; n < cumFreq[maxHufCodeBits - 2]; n++) {
		memset(hufCodeDemapPtr, sortHufCode[n].lit, 4);
		hufCodeDemapPtr += 4;
	}

	/* Fast demapping treatment for the case nbits == maxHufCodeBits-1, i.e., extBits==1 */
	for (; n < cumFreq[maxHufCodeBits - 1]; n++) {
		memset(hufCodeDemapPtr, sortHufCode[n].lit, 2);
		hufCodeDemapPtr += 2;
	}

	/* Fast demapping treatment for the case nbits == maxHufCodeBits, i.e., extBits==0 */
	for (; n < nEffLits; n++) {
		*hufCodeDemapPtr++ = sortHufCode[n].lit;
	}
}

void Build_Huffman_DecTableX1(const Uint32 hufCodeSize, const Uint32 maxHufCodeBits, Uint8* hufCodeBits, Huffman_DemapX1 *litHufDemapX1)
{
	Uint32 n, extBits;
	Uint32 i, nBits;
	Uint32 nEffLits;
	Huffman_DemapX1* hufDemapPtr, *hufDemapPtrEnd;
	if (0 == maxHufCodeBits) return;

	Huffman_DemapX1 sortHufCode[MAX_HufSize] = { 0 };
	Uint32 hufWtFreq[MAX_HufWeight + 1] = { 0 };   /* frequency of hufCodeBits */
	Uint32 cumFreq[MAX_HufWeight + 2];           /* cumulative frequency of hufWtFreq */
	
	for (i = 0; i < hufCodeSize; i++)
		hufWtFreq[hufCodeBits[i]]++;

	cumFreq[1] = 0;
	for (i = 1; i <= maxHufCodeBits; i++) {
		cumFreq[i + 1] = cumFreq[i] + hufWtFreq[i];
	}
	nEffLits = cumFreq[maxHufCodeBits + 1];
	cumFreq[0] = cumFreq[maxHufCodeBits + 1];

	/* radix sorting literals in increasing order of litHufBits, while maintaining the original order for ones with equal litHufBits */
	for (i = 0; i < hufCodeSize; i++) {
		nBits = hufCodeBits[i];
		sortHufCode[cumFreq[nBits]].lit = (Uint8)i;
		sortHufCode[cumFreq[nBits]++].nbits = (Uint8)nBits;
	}
	cumFreq[0] = 0;          /* reset original meaning */

	switch(maxHufCodeBits) {
	case 1:       /* nEffLits == 1 or 2 */
		litHufDemapX1[0] = sortHufCode[0];
		litHufDemapX1[1] = sortHufCode[1];
		return;
	case 2:       /* nEffLits == 3 or 4 */
		litHufDemapX1[0] = sortHufCode[0];	
		memcpy(litHufDemapX1+1, sortHufCode+(nEffLits-3), 3 * sizeof(Huffman_DemapX1));
		return;
	default:
		;
	}

	extBits = maxHufCodeBits - sortHufCode[0].nbits;
	hufDemapPtr = litHufDemapX1;
	if ( extBits >1 ) {
		hufDemapPtrEnd = litHufDemapX1 + (Uint32)(1 << extBits);
		do {
			*(hufDemapPtr++) = sortHufCode[0];
			*(hufDemapPtr++) = sortHufCode[0];
			*(hufDemapPtr++) = sortHufCode[0];
			*(hufDemapPtr++) = sortHufCode[0];
		} while (hufDemapPtr < hufDemapPtrEnd);
	}
	else {
		if (1 == extBits) {
			*(hufDemapPtr++) = sortHufCode[0];
		}
		*hufDemapPtr++ = sortHufCode[0];
	}
	

	for (n = 1; n < cumFreq[maxHufCodeBits - 3]; n++) {
		extBits = maxHufCodeBits - sortHufCode[n].nbits;
		hufDemapPtrEnd = hufDemapPtr + (Uint32)(1 << extBits);
		do {
			*(hufDemapPtr++) = sortHufCode[n];
			*(hufDemapPtr++) = sortHufCode[n];
			*(hufDemapPtr++) = sortHufCode[n];
			*(hufDemapPtr++) = sortHufCode[n];
		} while (hufDemapPtr < hufDemapPtrEnd);
	}

	while (n < cumFreq[maxHufCodeBits - 2]) {             /* Weight of maxHufCodeBits-1 is non-empty */
		*hufDemapPtr++ = sortHufCode[n];
		*hufDemapPtr++ = sortHufCode[n];
		*hufDemapPtr++ = sortHufCode[n];
		*hufDemapPtr++ = sortHufCode[n++];
	}

	while (n < cumFreq[maxHufCodeBits - 1]) {             /* Weight of maxHufCodeBits-1 is non-empty */
		*hufDemapPtr++ = sortHufCode[n];
		*hufDemapPtr++ = sortHufCode[n++];
	}

	memcpy(hufDemapPtr, &sortHufCode[n], (nEffLits - n) * sizeof(Huffman_DemapX1));
	
}

void Build_ExtHuffman_DecTableX1(const Uint32 hufCodeSize, const Uint32 maxHufCodeBits, Uint8* hufCodeBits, const ExtHuffman_Lit *extHufLit, ExtHuffman_DemapX1* litHufDemapX1)
{
	Uint32 i, n;
	Uint32 extBits, nBits;
	Uint32 nEffLits;
	ExtHuffman_DemapX1* hufDemapPtr, * hufDemapPtrEnd;
	if (0 == maxHufCodeBits) return;

	ExtHuffman_DemapX1 sortHufCode[MAX_HufSize] = { 0 };
	Uint32 hufWtFreq[MAX_HufWeight + 1] = { 0 };   /* frequency of hufCodeBits */
	Uint32 cumFreq[MAX_HufWeight + 2];           /* cumulative frequency of hufWtFreq */

	for (i = 0; i < hufCodeSize; i++)
		hufWtFreq[hufCodeBits[i]]++;

	cumFreq[0] = cumFreq[1] = 0;
	for (i = 1; i <= maxHufCodeBits; i++) {
		cumFreq[i + 1] = cumFreq[i] + hufWtFreq[i];
	}
	nEffLits = cumFreq[maxHufCodeBits + 1];
	cumFreq[0]  = cumFreq[maxHufCodeBits + 1];

	/* radix sorting literals in increasing order of litHufBits, while maintaining the original order for ones with equal litHufBits*/
	for (i = 0; i < hufCodeSize; i++) {
		nBits = hufCodeBits[i];
		sortHufCode[cumFreq[nBits]].extLit = extHufLit[i];
		sortHufCode[cumFreq[nBits]++].nbits = (Uint8)nBits;
	}

	switch (maxHufCodeBits) {
	case 1:       /* nEffLits == 1 or 2 */
		litHufDemapX1[0] = sortHufCode[0];
		litHufDemapX1[1] = sortHufCode[1];
		return;
	case 2:       /* nEffLits == 3 or 4 */
		litHufDemapX1[0] = sortHufCode[0];
		memcpy(litHufDemapX1 + 1, sortHufCode + (nEffLits - 3), 3 * sizeof(ExtHuffman_DemapX1));
		return;
	default:
		;
	}

	extBits = maxHufCodeBits - sortHufCode[0].nbits;
	hufDemapPtr = litHufDemapX1;
	if (extBits > 1) {
		hufDemapPtrEnd = litHufDemapX1 + (Uint32)(1 << extBits);
		do {
			*(hufDemapPtr++) = sortHufCode[0];
			*(hufDemapPtr++) = sortHufCode[0];
			*(hufDemapPtr++) = sortHufCode[0];
			*(hufDemapPtr++) = sortHufCode[0];
		} while (hufDemapPtr < hufDemapPtrEnd);
	}
	else {
		if (1 == extBits) {
			*(hufDemapPtr++) = sortHufCode[0];
		}
		*hufDemapPtr++ = sortHufCode[0];
	}


	for (n = 1; n < cumFreq[maxHufCodeBits - 3]; n++) {
		extBits = maxHufCodeBits - sortHufCode[n].nbits;
		hufDemapPtrEnd = hufDemapPtr + (Uint32)(1 << extBits);
		do {
			*(hufDemapPtr++) = sortHufCode[n];
			*(hufDemapPtr++) = sortHufCode[n];
			*(hufDemapPtr++) = sortHufCode[n];
			*(hufDemapPtr++) = sortHufCode[n];
		} while (hufDemapPtr < hufDemapPtrEnd);
	}

	while (n < cumFreq[maxHufCodeBits - 2]) {             /* Weight of maxHufCodeBits-1 is non-empty */
		*hufDemapPtr++ = sortHufCode[n];
		*hufDemapPtr++ = sortHufCode[n];
		*hufDemapPtr++ = sortHufCode[n];
		*hufDemapPtr++ = sortHufCode[n++];
	}

	while (n < cumFreq[maxHufCodeBits - 1]) {             /* Weight of maxHufCodeBits-1 is non-empty */
		*hufDemapPtr++ = sortHufCode[n];
		*hufDemapPtr++ = sortHufCode[n++];
	}

	memcpy(hufDemapPtr, &sortHufCode[n], (nEffLits - n) * sizeof(ExtHuffman_DemapX1));

}

Uint32 Read_Huffman_Header(Bit_Stream* bitStr, const Uint32 hufCodeSize, const Uint32 hufCodeCapBits, Uint8* hufCodeBits)
{
	Uint32 i, n;
	Uint32 hufLenBits, repLen;
	Uint32 maxHufCodeBits;
	register Bit_Stream bitStream = *bitStr;

	hufLenBits = N_Bits(hufCodeCapBits);
	BITStream_Read(bitStream, hufLenBits, hufCodeBits[0]);

	maxHufCodeBits = 0;
	for (i = 1; i < hufCodeSize; i++) {
		BITStream_Read(bitStream, hufLenBits, hufCodeBits[i]);
		maxHufCodeBits = Max(maxHufCodeBits, hufCodeBits[i]);
		if (hufCodeBits[i] == hufCodeBits[i - 1]) {
			BITStream_Read(bitStream, 2, repLen);        /* followed by repetative length */
			if (3 == repLen) {
				BITStream_Read(bitStream, 4, n);
				repLen += n;
				if (repLen == 3 + 15) {
					BITStream_Read(bitStream, 8, n);
					repLen += n;
				}
			}		
			memset(hufCodeBits + i+1, hufCodeBits[i], repLen);
			i += repLen;
		}
		BITStream_Read_Flush(bitStream);
	}
	*bitStr = bitStream;
	return maxHufCodeBits;
}

/* Read Huffman header whose weight sequence is further compressed by Huffman */
Uint32 Read_Huffman_Header_byHuffman(Bit_Stream* bitStr, Uint32 maxHufWtHufCodeBits, Huffman_DemapX1* hufWtHufCodeMapX1, const Uint32 hufCodeSize,  Uint8* hufCodeBits)
{
	Uint32 i, n, nBits, repLen;
	const Uint32 repHufZero = MAX_HufWeight + 2;  /* two special symbols: zero-run of length at least 2 */
	const Uint32 repHufLen = MAX_HufWeight + 1;   /* non-zero single-run of length at least 2 */
	Uint32 maxHufCodeBits;
	register Bit_Stream bitStream = *bitStr;
	Uint32 const remMaxHufWtHufBits = sizeof(bitStream.container) * 8 - maxHufWtHufCodeBits;

	maxHufCodeBits = 0;
	for (i = 0; i < hufCodeSize; ) {
		BITStream_Read_ExtHufX1(bitStream, remMaxHufWtHufBits, hufWtHufCodeMapX1, nBits);
		if( nBits<repHufLen)  {
			hufCodeBits[i++] = (Uint8)nBits;
			maxHufCodeBits = max(maxHufCodeBits, nBits);
		}
		else if (nBits == repHufZero) {
			BITStream_Read(bitStream, 3, n);
			repLen = 2 + n;
			if (repLen == 9) {
				BITStream_Read(bitStream, 6, n);
				repLen += n;
				if (n == 63) {
					BITStream_Read(bitStream, 8, n);
					repLen += n;
				}
			}
			memset(hufCodeBits + i, 0, repLen);
			i += repLen;
		}
		else  {                    /* (nBits == repHufLen) */
			BITStream_Read(bitStream, 2, n);
			repLen = 2 + n;
			if (repLen == 5) {
				BITStream_Read(bitStream, 4, n);
				repLen += n;
				if (n == 15) {
					BITStream_Read(bitStream, 8, n);
					repLen += n;
				}
			}
			memset(hufCodeBits + i, i ? hufCodeBits[i - 1] : 0, repLen);   /* a repeat cannot come first in a valid header */
			i += repLen;
		}
		
		BITStream_Read_Flush(bitStream);
	}		

	*bitStr = bitStream;
	return maxHufCodeBits;
}

ForceInlineTemplate Uint32 Huffman_DecodeStreamX0(Bit_Stream bitStream, Uint8* dest, const Uint8* destEnd, const Uint32 maxLitHufCodeBits, Uint8* litHufCodeBits, Uint8* litHufCodeDemap)
{
	Uint8* destPtr = (Uint8*)dest;
	const Uint32 remMaxLitHufBits = sizeof(bitStream.container) * 8 - maxLitHufCodeBits;

	while (destPtr < destEnd - 3) {
		BITStream_Read_HufX0(bitStream, remMaxLitHufBits, litHufCodeBits, litHufCodeDemap, destPtr); 
		BITStream_Read_HufX0(bitStream, remMaxLitHufBits, litHufCodeBits, litHufCodeDemap, destPtr);
		BITStream_Read_HufX0(bitStream, remMaxLitHufBits, litHufCodeBits, litHufCodeDemap, destPtr);
		BITStream_Read_HufX0(bitStream, remMaxLitHufBits, litHufCodeBits, litHufCodeDemap, destPtr);
		BITStream_Read_Flush(bitStream);
	}

	switch (destEnd - destPtr) {
	case 3:
		BITStream_Read_HufX0(bitStream, remMaxLitHufBits, litHufCodeBits, litHufCodeDemap, destPtr);
	case 2:
		BITStream_Read_HufX0(bitStream, remMaxLitHufBits, litHufCodeBits, litHufCodeDemap, destPtr);
	case 1:
		BITStream_Read_HufX0(bitStream, remMaxLitHufBits, litHufCodeBits, litHufCodeDemap, destPtr);
	default:
		;
	}

	return (Uint32)(destEnd - dest);
}

ForceInlineTemplate Uint32 Huffman_Decompress4X0_Kernel_Body(void* source, void* dest, Uint32 destSize, const Uint32 maxLitHufCodeBits, Uint8* litHufCodeBits, Uint8* litHufDemap)
{
	Bit_Stream bitStream0, bitStream1, bitStream2, bitStream3;
	Uint32 destSegSize = (destSize >> 4) << 2;
	Uint8* destSegPtr0 = (Uint8*)dest;
	Uint8* destSegPtr1 = (Uint8*)dest + destSegSize;
	Uint8* destSegPtr2 = (Uint8*)dest + 2 * destSegSize;
	Uint8* destSegPtr3 = (Uint8*)dest + 3 * destSegSize;
	const Uint8* const destSegEnd0 = destSegPtr1;
	const Uint8* const destEnd = (Uint8*)dest + destSize;
	const Uint32 remMaxLitHufBits = sizeof(bitStream0.container) * 8 - maxLitHufCodeBits;

	Uint8* srcPtr = (Uint8*)source;
	const Uint32 srcSegSize0 = MemReadLE2(srcPtr);
	const Uint32 srcSegSize1 = MemReadLE2(srcPtr + 2);
	const Uint32 srcSegSize2 = MemReadLE2(srcPtr + 4);
	srcPtr += 6;
	bitStream0.nUsedBits = bitStream1.nUsedBits = bitStream2.nUsedBits = bitStream3.nUsedBits = 0;
	bitStream0.container = MemReadBE8(srcPtr);
	bitStream0.streamPtr = srcPtr;
	bitStream1.container = MemReadBE8(srcPtr + srcSegSize0);
	bitStream1.streamPtr = srcPtr + srcSegSize0;
	bitStream2.container = MemReadBE8(srcPtr + srcSegSize1);
	bitStream2.streamPtr = srcPtr + srcSegSize1;
	bitStream3.container = MemReadBE8(srcPtr + srcSegSize2);
	bitStream3.streamPtr = srcPtr + srcSegSize2;

	while (destSegPtr0 < destSegEnd0 ) {                             /* Note the literal lengths of segment 0, 1, 2, are equal and divide 4 */
		BITStream_Read_HufX0(bitStream0, remMaxLitHufBits, litHufCodeBits, litHufDemap, destSegPtr0); 
		BITStream_Read_HufX0(bitStream1, remMaxLitHufBits, litHufCodeBits, litHufDemap, destSegPtr1); 
		BITStream_Read_HufX0(bitStream2, remMaxLitHufBits, litHufCodeBits, litHufDemap, destSegPtr2); 
		BITStream_Read_HufX0(bitStream3, remMaxLitHufBits, litHufCodeBits, litHufDemap, destSegPtr3); 

		BITStream_Read_HufX0(bitStream0, remMaxLitHufBits, litHufCodeBits, litHufDemap, destSegPtr0);
		BITStream_Read_HufX0(bitStream1, remMaxLitHufBits, litHufCodeBits, litHufDemap, destSegPtr1);
		BITStream_Read_HufX0(bitStream2, remMaxLitHufBits, litHufCodeBits, litHufDemap, destSegPtr2);
		BITStream_Read_HufX0(bitStream3, remMaxLitHufBits, litHufCodeBits, litHufDemap, destSegPtr3);

		BITStream_Read_HufX0(bitStream0, remMaxLitHufBits, litHufCodeBits, litHufDemap, destSegPtr0);
		BITStream_Read_HufX0(bitStream1, remMaxLitHufBits, litHufCodeBits, litHufDemap, destSegPtr1);
		BITStream_Read_HufX0(bitStream2, remMaxLitHufBits, litHufCodeBits, litHufDemap, destSegPtr2);
		BITStream_Read_HufX0(bitStream3, remMaxLitHufBits, litHufCodeBits, litHufDemap, destSegPtr3);

		BITStream_Read_HufX0(bitStream0, remMaxLitHufBits, litHufCodeBits, litHufDemap, destSegPtr0);
		BITStream_Read_HufX0(bitStream1, remMaxLitHufBits, litHufCodeBits, litHufDemap, destSegPtr1);
		BITStream_Read_HufX0(bitStream2, remMaxLitHufBits, litHufCodeBits, litHufDemap, destSegPtr2);
		BITStream_Read_HufX0(bitStream3, remMaxLitHufBits, litHufCodeBits, litHufDemap, destSegPtr3);

		BITStream_Read_Flush(bitStream0);
		BITStream_Read_Flush(bitStream1);
		BITStream_Read_Flush(bitStream2);
		BITStream_Read_Flush(bitStream3);
	}

	/* The first three segments have been completed */
	Huffman_DecodeStreamX0(bitStream3, destSegPtr3, destEnd, maxLitHufCodeBits, litHufCodeBits, litHufDemap);

	return destSize;
}

/* Each decode kernel is also compiled for BMI2, whose shifts by a register count take one micro-op and leave the
   flags alone; the faster copy is chosen at run time, as zstd does. */
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#  define HUF_DYNAMIC_BMI2 1
#  define HUF_BMI2_KERNEL(fun, ret, params, args)  static __attribute__((target("bmi,bmi2,lzcnt"))) ret fun##_Bmi2 params { return fun##_Body args; }
static int HUF_Cpu_Bmi2(void)
{
	static int has = -1;
	if (has < 0) {
		__builtin_cpu_init();
		has = __builtin_cpu_supports("bmi2") != 0;
	}
	return has;
}
#  define HUF_KERNEL(fun)  (HUF_Cpu_Bmi2() ? fun##_Bmi2 : fun)
#else
#  define HUF_DYNAMIC_BMI2 0
#  define HUF_BMI2_KERNEL(fun, ret, params, args)
#  define HUF_KERNEL(fun)  fun
#endif

#define HUFFMAN_DECODERX0_GEN(fun)                                                        \
    static Uint32 fun(void* source, void* dest, Uint32 destSize, Uint32 maxLitHufCodeBits, Uint8* litHufCodeBits, Uint8* litHufDemap)   \
    {                                                                       \
        return fun##_Body(source, dest, destSize, maxLitHufCodeBits, litHufCodeBits, litHufDemap);             \
    }                                                                       \
    HUF_BMI2_KERNEL(fun, Uint32, (void* source, void* dest, Uint32 destSize, Uint32 maxLitHufCodeBits, Uint8* litHufCodeBits, Uint8* litHufDemap), \
                    (source, dest, destSize, maxLitHufCodeBits, litHufCodeBits, litHufDemap))


HUFFMAN_DECODERX0_GEN(Huffman_Decompress4X0_Kernel)


/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~  Decompression Type X1 ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
ForceInlineTemplate Uint32 Huffman_DecodeStreamX1(Bit_Stream bitStream, Uint8* dest, const Uint8* destEnd, const Uint32 maxLitHufCodeBits, Huffman_DemapX1* litHufDemapX1)
{
	Uint8* destPtr = (Uint8*)dest;
	const Uint32 remMaxLitHufBits = sizeof(bitStream.container) * 8 - maxLitHufCodeBits;
	
	while (destPtr < destEnd - 3) {
		BITStream_Read_HufX1(bitStream, remMaxLitHufBits, litHufDemapX1, destPtr); 
		BITStream_Read_HufX1(bitStream, remMaxLitHufBits, litHufDemapX1, destPtr);
		BITStream_Read_HufX1(bitStream, remMaxLitHufBits, litHufDemapX1, destPtr);
		BITStream_Read_HufX1(bitStream, remMaxLitHufBits, litHufDemapX1, destPtr);
		BITStream_Read_Flush(bitStream);
	}

	switch (destEnd - destPtr) {
	case 3:
		BITStream_Read_HufX1(bitStream, remMaxLitHufBits, litHufDemapX1, destPtr);
	case 2:
		BITStream_Read_HufX1(bitStream, remMaxLitHufBits, litHufDemapX1, destPtr);
	case 1:
		BITStream_Read_HufX1(bitStream, remMaxLitHufBits, litHufDemapX1, destPtr);
	default:
		;
	}
		
	return (Uint32)(destEnd - dest);
}

ForceInlineTemplate Uint32 Huffman_DecompressX1_Kernel_Body(void* source, void* dest, Uint32 destSize, Uint32 maxLitHufCodeBits, Huffman_DemapX1* litHufDemapX1)
{
	Bit_Stream bitStream = { MemReadBE8(source), 0, (Uint8*)source };
	const Uint8* const destEnd = (Uint8*)dest + destSize;

	return Huffman_DecodeStreamX1(bitStream, dest, destEnd, maxLitHufCodeBits, litHufDemapX1);
	
}

ForceInlineTemplate Uint32 Huffman_Decompress4X1_Kernel_Body(void* source, void* dest, Uint32 destSize, const Uint32 maxLitHufCodeBits, Huffman_DemapX1* litHufDemapX1)
{
	register Bit_Stream bitStream0, bitStream1, bitStream2, bitStream3;
	
	Uint32 destSegSize = (destSize>>4) <<2;
	Uint8* destSegPtr0 = (Uint8*)dest;
	Uint8* destSegPtr1 = (Uint8*)dest + destSegSize;
	Uint8* destSegPtr2 = (Uint8*)dest + 2 * destSegSize;
	Uint8* destSegPtr3 = (Uint8*)dest + 3 * destSegSize;
	const Uint8* const destSegEnd0 = destSegPtr1;
	const Uint8* const destEnd = (Uint8*)dest + destSize;
	const Uint32 remMaxLitHufBits = sizeof(bitStream0.container) * 8 - maxLitHufCodeBits;

	Uint8* srcPtr = (Uint8*)source;
	const Uint32 srcSegSize0 = MemReadLE2(srcPtr);
	const Uint32 srcSegSize1 = MemReadLE2(srcPtr + 2);
	const Uint32 srcSegSize2 = MemReadLE2(srcPtr + 4);
	srcPtr += 6;
	bitStream0.nUsedBits = bitStream1.nUsedBits = bitStream2.nUsedBits = bitStream3.nUsedBits = 0;
	bitStream0.container = MemReadBE8(srcPtr);
	bitStream0.streamPtr = srcPtr;
	bitStream1.container = MemReadBE8(srcPtr + srcSegSize0);
	bitStream1.streamPtr = srcPtr + srcSegSize0;
	bitStream2.container = MemReadBE8(srcPtr + srcSegSize1);
	bitStream2.streamPtr = srcPtr + srcSegSize1;
	bitStream3.container = MemReadBE8(srcPtr + srcSegSize2);
	bitStream3.streamPtr = srcPtr + srcSegSize2;
	
	while (destSegPtr0 < destSegEnd0) {                             /* Note the literal lengths of segment 0, 1, 2, are equal and divide 4 */
		BITStream_Read_HufX1(bitStream0, remMaxLitHufBits, litHufDemapX1, destSegPtr0);  
		BITStream_Read_HufX1(bitStream1, remMaxLitHufBits, litHufDemapX1, destSegPtr1); 
		BITStream_Read_HufX1(bitStream2, remMaxLitHufBits, litHufDemapX1, destSegPtr2);   
		BITStream_Read_HufX1(bitStream3, remMaxLitHufBits, litHufDemapX1, destSegPtr3);  

		BITStream_Read_HufX1(bitStream0, remMaxLitHufBits, litHufDemapX1, destSegPtr0);
		BITStream_Read_HufX1(bitStream1, remMaxLitHufBits, litHufDemapX1, destSegPtr1);
		BITStream_Read_HufX1(bitStream2, remMaxLitHufBits, litHufDemapX1, destSegPtr2);
		BITStream_Read_HufX1(bitStream3, remMaxLitHufBits, litHufDemapX1, destSegPtr3);

		BITStream_Read_HufX1(bitStream0, remMaxLitHufBits, litHufDemapX1, destSegPtr0);
		BITStream_Read_HufX1(bitStream1, remMaxLitHufBits, litHufDemapX1, destSegPtr1);
		BITStream_Read_HufX1(bitStream2, remMaxLitHufBits, litHufDemapX1, destSegPtr2);
		BITStream_Read_HufX1(bitStream3, remMaxLitHufBits, litHufDemapX1, destSegPtr3);

		BITStream_Read_HufX1(bitStream0, remMaxLitHufBits, litHufDemapX1, destSegPtr0);
		BITStream_Read_HufX1(bitStream1, remMaxLitHufBits, litHufDemapX1, destSegPtr1);
		BITStream_Read_HufX1(bitStream2, remMaxLitHufBits, litHufDemapX1, destSegPtr2);
		BITStream_Read_HufX1(bitStream3, remMaxLitHufBits, litHufDemapX1, destSegPtr3);

		BITStream_Read_Flush(bitStream0);
		BITStream_Read_Flush(bitStream1);
		BITStream_Read_Flush(bitStream2);
		BITStream_Read_Flush(bitStream3);
	} 
		

	/* By our designated setup, the first three segments have been completed */
	Huffman_DecodeStreamX1(bitStream3, destSegPtr3, destEnd, maxLitHufCodeBits, litHufDemapX1);

	return destSize;
}

#define HUFFMAN_DECODERX1_GEN(fun)                                                        \
    static Uint32 fun(void* source, void* dest, Uint32 destSize, Uint32 maxLitHufCodeBits, Huffman_DemapX1 *litHufDemapX1)   \
    {                                                                       \
        return fun##_Body(source, dest, destSize, maxLitHufCodeBits, litHufDemapX1);             \
    }                                                                       \
    HUF_BMI2_KERNEL(fun, Uint32, (void* source, void* dest, Uint32 destSize, Uint32 maxLitHufCodeBits, Huffman_DemapX1 *litHufDemapX1), \
                    (source, dest, destSize, maxLitHufCodeBits, litHufDemapX1))


HUFFMAN_DECODERX1_GEN(Huffman_DecompressX1_Kernel)
HUFFMAN_DECODERX1_GEN(Huffman_Decompress4X1_Kernel)


/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ X2:  Decompress up to two symbols per look-up  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~  */
/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

typedef struct {
	Uint8 lit;
	Uint8 nbits;
	Uint16 code;
} SortHufCodeX2_Str;

void Build_Huffman_DecTableX2(const Uint32 hufCodeSize, const Uint32 maxHufCodeBits, Uint8* hufCodeBits, Huffman_DemapX2* litHufDemapX2, const Uint32 hufDemapBitsX2)
{
	Uint32 i, n;
	Uint32 extBits, nBits, extLen;
	Uint32 nEffLits;
	SortHufCodeX2_Str sortHufCode[MAX_HufSize] = { 0 };
	Huffman_DemapX2* hufDecTabPtr1, *hufDecTabPtr, *hufDecTabEnd;
	Huffman_DemapX2  hufDecTabEntry1, hufDecTabEntry2;
	hufDecTabEntry1.length = 1;
	hufDecTabEntry2.length = 2;


	Uint32 hufWtFreq[MAX_HufWeight + 1] = { 0 };   /* frequency of hufCodeBits */
	Uint32 cumFreq[MAX_HufWeight + 2];           /* cumulative frequency of hufWtFreq */

	assert(maxHufCodeBits <= hufDemapBitsX2);
	for (i = 0; i < hufCodeSize; i++)
		hufWtFreq[hufCodeBits[i]]++;

	cumFreq[1] = 0;
	for (i = 1; i <= maxHufCodeBits; i++) {
		cumFreq[i + 1] = cumFreq[i] + hufWtFreq[i];
	}
	nEffLits = cumFreq[maxHufCodeBits + 1];
	cumFreq[0] = cumFreq[maxHufCodeBits + 1];

	/* radix sorting literals in increasing order of litHufBits, while maintaining the original order for ones with equal litHufBits*/
	for (i = 0; i < hufCodeSize; i++) {
		nBits = hufCodeBits[i];
		sortHufCode[cumFreq[nBits]].lit = (Uint8)i;
		sortHufCode[cumFreq[nBits]++].nbits = (Uint8)nBits;
	}
	cumFreq[0] = 0;

	/* Special handling of degenerated one-bit Huffman codes */
	if (1 == maxHufCodeBits) {
		litHufDemapX2[0].lit[0] = sortHufCode[0].lit;
		litHufDemapX2[0].lit[1] = sortHufCode[0].lit;
		litHufDemapX2[0].nbits = 2;
		litHufDemapX2[0].length = 2;

		litHufDemapX2[1].lit[0] = sortHufCode[0].lit;
		litHufDemapX2[1].lit[1] = sortHufCode[1].lit;
		litHufDemapX2[1].nbits = 2;
		litHufDemapX2[1].length = 2;

		litHufDemapX2[2].lit[0] = sortHufCode[1].lit;
		litHufDemapX2[2].lit[1] = sortHufCode[0].lit;
		litHufDemapX2[2].nbits = 2;
		litHufDemapX2[2].length = 2;

		litHufDemapX2[3].lit[0] = sortHufCode[1].lit;
		litHufDemapX2[3].lit[1] = sortHufCode[1].lit;
		litHufDemapX2[3].nbits = 2;
		litHufDemapX2[3].length = 2;
		return;
	}

	sortHufCode[0].code = 0;
	for (n = 1; n < nEffLits; n++) {
		sortHufCode[n].code = (sortHufCode[n-1].code + 1) << (sortHufCode[n].nbits - sortHufCode[n-1].nbits);
	}
	
	for (n = 0; n < cumFreq[maxHufCodeBits - 1]; n++) {
		extBits = hufDemapBitsX2 - sortHufCode[n].nbits;
		hufDecTabPtr1 = litHufDemapX2 + (Uint32)(sortHufCode[n].code << extBits);
		hufDecTabPtr = hufDecTabPtr1;

		hufDecTabEntry1.lit[0] = sortHufCode[n].lit;
		hufDecTabEntry1.nbits = sortHufCode[n].nbits;
		for (i = 0; i<nEffLits && sortHufCode[i].nbits <= extBits; i++) {
			hufDecTabEntry2.lit[0] = hufDecTabEntry1.lit[0];
			hufDecTabEntry2.lit[1] = sortHufCode[i].lit;
			hufDecTabEntry2.nbits = hufDecTabEntry1.nbits + sortHufCode[i].nbits;
			hufDecTabPtr = hufDecTabPtr1 + (Uint32)(sortHufCode[i].code << (extBits - sortHufCode[i].nbits));
			extLen = 1 << (extBits - sortHufCode[i].nbits);
			hufDecTabEnd = hufDecTabPtr + extLen;
			do {
				*hufDecTabPtr++ = hufDecTabEntry2;
				*hufDecTabPtr++ = hufDecTabEntry2;
				*hufDecTabPtr++ = hufDecTabEntry2;
				*hufDecTabPtr++ = hufDecTabEntry2;
			} while (hufDecTabPtr < hufDecTabEnd);
			hufDecTabPtr = hufDecTabEnd;
		}

		hufDecTabEnd = hufDecTabPtr1 + (Uint32)(1 << extBits);
		do {
			*hufDecTabPtr++ = hufDecTabEntry1;
			*hufDecTabPtr++ = hufDecTabEntry1;
			*hufDecTabPtr++ = hufDecTabEntry1;
			*hufDecTabPtr++ = hufDecTabEntry1;
		} while (hufDecTabPtr < hufDecTabEnd);
	}

	/* We enforce the longest weight literal is not followed by another literal in the table, even if possible */
	extBits = hufDemapBitsX2 - maxHufCodeBits;
	hufDecTabEntry1.nbits = (Uint8)maxHufCodeBits;
	assert(hufDecTabEntry1.length == 1);
	switch( extBits ){
	case 0:
		hufDecTabPtr = litHufDemapX2 + sortHufCode[n].code;
		for (; n < nEffLits; n++) {
			hufDecTabEntry1.lit[0] = sortHufCode[n].lit;
			*hufDecTabPtr++ = hufDecTabEntry1;
		}
		break;
	case 1:
		hufDecTabPtr = litHufDemapX2 + (sortHufCode[n].code << 1);
		for (; n < nEffLits; n++) {
			hufDecTabEntry1.lit[0] = sortHufCode[n].lit;
			*hufDecTabPtr++ = hufDecTabEntry1;
			*hufDecTabPtr++ = hufDecTabEntry1;
		}
		break;
	default:
		hufDecTabPtr = litHufDemapX2 + (sortHufCode[n].code << extBits);
		for (; n < nEffLits; n++) {
			hufDecTabEntry1.lit[0] = sortHufCode[n].lit;
			hufDecTabEnd = hufDecTabPtr + (Uint32)(1 << extBits);
			do {
				*hufDecTabPtr++ = hufDecTabEntry1;
				*hufDecTabPtr++ = hufDecTabEntry1;
				*hufDecTabPtr++ = hufDecTabEntry1;
				*hufDecTabPtr++ = hufDecTabEntry1;
			} while (hufDecTabPtr < hufDecTabEnd);
		}
	}

}

ForceInlineTemplate Uint32 Huffman_DecodeStreamX2(Bit_Stream* bitStr, Uint8* dest, const Uint8* destEnd, const Uint32 hufDemapBitsX2, Huffman_DemapX2* litHufDemapX2)
{
	Uint8* destPtr = (Uint8*)dest;
	register Bit_Stream bitStream = *bitStr;
	const Uint32 remHufDemapBitsX2 = sizeof(bitStream.container) * 8 - hufDemapBitsX2;
	
	while (destPtr < destEnd - 7) {   /* Expand 4 reads then flush the bit container */
		BITStream_Read_HufX2(destPtr, bitStream, remHufDemapBitsX2, litHufDemapX2);
		BITStream_Read_HufX2(destPtr, bitStream, remHufDemapBitsX2, litHufDemapX2);
		BITStream_Read_HufX2(destPtr, bitStream, remHufDemapBitsX2, litHufDemapX2);
		BITStream_Read_HufX2(destPtr, bitStream, remHufDemapBitsX2, litHufDemapX2);

		BITStream_Read_Flush(bitStream);
	}

	/*~~~~~~~~~~~~~~~~~~ Decompress the remaining up to 7 literals ~~~~~~~~~~~~~~~~~~~~~~~~*/
	switch ((destEnd - destPtr) / 2) {
	case 3:
		BITStream_Read_HufX2(destPtr, bitStream, remHufDemapBitsX2, litHufDemapX2);
	case 2:
		BITStream_Read_HufX2(destPtr, bitStream, remHufDemapBitsX2, litHufDemapX2);
	case 1:
		BITStream_Read_HufX2(destPtr, bitStream, remHufDemapBitsX2, litHufDemapX2);
		BITStream_Read_Flush(bitStream);
	default:
		;
	}
		

	while (destPtr < destEnd - 1) {
		BITStream_Read_HufX2(destPtr, bitStream, remHufDemapBitsX2, litHufDemapX2);
	}

	if (destPtr < destEnd ) {  /* decompress the last literal while avoiding overflow */
		BITStream_Read_HufEndX2(destPtr, bitStream, remHufDemapBitsX2, litHufDemapX2);
	}

	return (Uint32)(destEnd - dest);
}

ForceInlineTemplate Uint32 Huffman_Decompress4X2_Kernel_Body(void* source, void* dest, Uint32 destSize, const Uint32 hufDemapBitsX2, Huffman_DemapX2* litHufDemapX2)
{
	Uint32 destSegSize = (destSize >>4) <<2;
	Bit_Stream bitStream0, bitStream1, bitStream2, bitStream3;
	Uint8* destSegPtr0 = (Uint8*)dest;
	Uint8* destSegPtr1 = (Uint8*)dest + destSegSize;
	Uint8* destSegPtr2 = (Uint8*)dest + 2 * destSegSize;
	Uint8* destSegPtr3 = (Uint8*)dest + 3 * destSegSize;
	const Uint8* destSegEnd0 = destSegPtr1;
	const Uint8* destSegEnd1 = destSegPtr2;
	const Uint8* destSegEnd2 = destSegPtr3;
	const Uint8* destEnd = (Uint8*)dest + destSize;
	const Uint32 remHufDemapBitsX2 = sizeof(bitStream0.container) * 8 - hufDemapBitsX2;

	Uint8* srcPtr = (Uint8*)source;
	const Uint32 srcSegSize0 = MemReadLE2(srcPtr);
	const Uint32 srcSegSize1 = MemReadLE2(srcPtr+2);
	const Uint32 srcSegSize2 = MemReadLE2(srcPtr + 4);
	srcPtr += 6;
	bitStream0.nUsedBits = bitStream1.nUsedBits = bitStream2.nUsedBits = bitStream3.nUsedBits = 0;
	bitStream0.container = MemReadBE8(srcPtr);
	bitStream0.streamPtr = srcPtr;
	bitStream1.container = MemReadBE8(srcPtr + srcSegSize0);
	bitStream1.streamPtr = srcPtr + srcSegSize0;
	bitStream2.container = MemReadBE8(srcPtr + srcSegSize1);
	bitStream2.streamPtr = srcPtr + srcSegSize1;
	bitStream3.container = MemReadBE8(srcPtr + srcSegSize2);
	bitStream3.streamPtr = srcPtr + srcSegSize2;

	do {
		BITStream_Read_HufX2(destSegPtr0, bitStream0, remHufDemapBitsX2, litHufDemapX2);
		BITStream_Read_HufX2(destSegPtr1, bitStream1, remHufDemapBitsX2, litHufDemapX2);
		BITStream_Read_HufX2(destSegPtr2, bitStream2, remHufDemapBitsX2, litHufDemapX2);
		BITStream_Read_HufX2(destSegPtr3, bitStream3, remHufDemapBitsX2, litHufDemapX2);

		BITStream_Read_HufX2(destSegPtr0, bitStream0, remHufDemapBitsX2, litHufDemapX2);
		BITStream_Read_HufX2(destSegPtr1, bitStream1, remHufDemapBitsX2, litHufDemapX2);
		BITStream_Read_HufX2(destSegPtr2, bitStream2, remHufDemapBitsX2, litHufDemapX2);
		BITStream_Read_HufX2(destSegPtr3, bitStream3, remHufDemapBitsX2, litHufDemapX2);

		BITStream_Read_HufX2(destSegPtr0, bitStream0, remHufDemapBitsX2, litHufDemapX2);
		BITStream_Read_HufX2(destSegPtr1, bitStream1, remHufDemapBitsX2, litHufDemapX2);
		BITStream_Read_HufX2(destSegPtr2, bitStream2, remHufDemapBitsX2, litHufDemapX2);
		BITStream_Read_HufX2(destSegPtr3, bitStream3, remHufDemapBitsX2, litHufDemapX2);

		BITStream_Read_HufX2(destSegPtr0, bitStream0, remHufDemapBitsX2, litHufDemapX2);
		BITStream_Read_HufX2(destSegPtr1, bitStream1, remHufDemapBitsX2, litHufDemapX2);
		BITStream_Read_HufX2(destSegPtr2, bitStream2, remHufDemapBitsX2, litHufDemapX2);
		BITStream_Read_HufX2(destSegPtr3, bitStream3, remHufDemapBitsX2, litHufDemapX2);

		BITStream_Read_Flush(bitStream0);
		BITStream_Read_Flush(bitStream1);
		BITStream_Read_Flush(bitStream2);
		BITStream_Read_Flush(bitStream3);
	} while (destSegPtr0 < destSegEnd0 - 7 && destSegPtr1 < destSegEnd1 - 7 && destSegPtr2 < destSegEnd2 - 7 && destSegPtr3 < destEnd - 7);
		
	Huffman_DecodeStreamX2(&bitStream0, destSegPtr0, destSegEnd0, hufDemapBitsX2, litHufDemapX2);
	Huffman_DecodeStreamX2(&bitStream1, destSegPtr1, destSegEnd1, hufDemapBitsX2, litHufDemapX2);
	Huffman_DecodeStreamX2(&bitStream2, destSegPtr2, destSegEnd2, hufDemapBitsX2, litHufDemapX2);
	Huffman_DecodeStreamX2(&bitStream3, destSegPtr3, destEnd, hufDemapBitsX2, litHufDemapX2);

	return destSize;
}

#define HUFFMAN_DECODERX2_GEN(fun)                                                                                                                  \
    static Uint32 fun(void* source, void* dest, Uint32 destSize, const Uint32 hufDemapBitsX2, Huffman_DemapX2* litHufDemapX2)   \
    {                                                                                                                                               \
        return fun##_Body(source, dest, destSize, hufDemapBitsX2, litHufDemapX2);                                                            \
    }                                                                                                                                               \
    HUF_BMI2_KERNEL(fun, Uint32, (void* source, void* dest, Uint32 destSize, const Uint32 hufDemapBitsX2, Huffman_DemapX2* litHufDemapX2), \
                    (source, dest, destSize, hufDemapBitsX2, litHufDemapX2))

HUFFMAN_DECODERX2_GEN(Huffman_Decompress4X2_Kernel)

typedef struct { int tableTime; int decode256Time; } Algo_Time_Str;
static const Algo_Time_Str algoTime[16 /* Quantization */][2 /* single, double */] =
{
	/* single, double */
	{{0,0}, {1,1}},  /* Q==0 : impossible */
	{{0,0}, {1,1}},  /* Q==1 : impossible */
	{{  38,130}, {1313, 74}},   /* Q == 2 : 12-18% */
	{{ 448,128}, {1353, 74}},   /* Q == 3 : 18-25% */
	{{ 556,128}, {1353, 74}},   /* Q == 4 : 25-32% */
	{{ 714,128}, {1418, 74}},   /* Q == 5 : 32-38% */
	{{ 883,128}, {1437, 74}},   /* Q == 6 : 38-44% */
	{{ 897,128}, {1515, 75}},   /* Q == 7 : 44-50% */
	{{ 926,128}, {1613, 75}},   /* Q == 8 : 50-56% */
	{{ 947,128}, {1729, 77}},   /* Q == 9 : 56-62% */
	{{1107,128}, {2083, 81}},   /* Q ==10 : 62-69% */
	{{1177,128}, {2379, 87}},   /* Q ==11 : 69-75% */
	{{1242,128}, {2415, 93}},   /* Q ==12 : 75-81% */
	{{1349,128}, {2644,106}},   /* Q ==13 : 81-87% */
	{{1455,128}, {2422,124}},   /* Q ==14 : 87-93% */
	{{ 722,128}, {1891,145}},   /* Q ==15 : 93-99% */
};

/** It determines which decoder is likely to decode faster,
 *  based on a set of pre-computed metrics.
 * @return : 0==Huffman_Decompress4X1, 1==Huffman_Decompress4X2 */
int Huffman_Select_Decompressor(Uint32 cmprSize, Uint32 srcSize, Uint32 maxHufBits, Uint32 *hufDemapBitsX2)
{
	/* decoder timing evaluation */
	Uint32 const Q = (srcSize >= cmprSize) ? 15 : (int)(srcSize * 16 / cmprSize);   /* Q < 16 */

	/* Heuristically Determine optimal hufDemapBitsX2, Q denotes average length of two literals */
	*hufDemapBitsX2 = min(max(Q, maxHufBits+2), MAX_HufWeight);
	if (1 == maxHufBits) *hufDemapBitsX2 = 2;
	int const D256 = (int)(cmprSize >> 8);
	int const DTime0 = algoTime[Q][0].tableTime + (algoTime[Q][0].decode256Time * D256);
	int DTime1 = algoTime[Q][1].tableTime + (algoTime[Q][1].decode256Time * D256);
	DTime1 += DTime1 *(*hufDemapBitsX2-maxHufBits)>>3;  /* advantage to algorithm using less memory, to reduce cache eviction */
	return DTime1 < DTime0;

}

void Huffman_Decompress_Block_Body(Uint8* litHufCodeBits, int nLits, Uint32 maxLitHufBits, int algId, Uint8* cmprBuffer, Uint32 cmprSize, Uint8* decBuffer, const Uint32 decSize)
{
	assert(maxLitHufBits <= MAX_HufWeight);
	assert(algId != 1 || decSize >= MinStream4XSize);
	Uint32 hufDemapBitsX2 = maxLitHufBits+0;
	if (algId < 0 || algId>2)
		algId = 1+ Huffman_Select_Decompressor(cmprSize, decSize, maxLitHufBits, &hufDemapBitsX2);

	Uint8 *litHufCodeDemap;
	Huffman_DemapX1* litHufCodeDemapX1;
	Huffman_DemapX2* litHufCodeDemapX2;

	/*litHufCodeDemap = (Uint8*)malloc((1 << maxLitHufBits));    
	Build_Huffman_DecTableX0(nLits, maxLitHufBits, litHufCodeBits, litHufCodeDemap);
	litHufCodeDemapX1 = (Huffman_DemapX1*)malloc((1 << maxLitHufBits) * sizeof(Huffman_DemapX1));
	Build_Huffman_DecTableX1(nLits, maxLitHufBits, litHufCodeBits, litHufCodeDemapX1);
	for (int i = 0; i < 1 << maxLitHufBits; i++) {
		if (litHufCodeDemapX1[i].lit != litHufCodeDemap[i]) {
			fprintf(stderr, "X1 decompression table is incorrect at [%d]\n", i);
		}
	}*/

	switch (algId) {
	case 0: 
		 litHufCodeDemap = (Uint8*)malloc( (Uint32)(1 << maxLitHufBits));
		Build_Huffman_DecTableX0(nLits, maxLitHufBits, litHufCodeBits, litHufCodeDemap);
		HUF_KERNEL(Huffman_Decompress4X0_Kernel)(cmprBuffer, decBuffer, decSize, maxLitHufBits, litHufCodeBits, litHufCodeDemap);
		free(litHufCodeDemap);
		return;
	case 1:
		litHufCodeDemapX1 = (Huffman_DemapX1*)malloc( (Uint32)(1 << maxLitHufBits) * sizeof(Huffman_DemapX1));
		Build_Huffman_DecTableX1(nLits, maxLitHufBits, litHufCodeBits, litHufCodeDemapX1);
		if (decSize >= MinStream4XSize)
			HUF_KERNEL(Huffman_Decompress4X1_Kernel)(cmprBuffer, decBuffer, decSize, maxLitHufBits, litHufCodeDemapX1);
		else HUF_KERNEL(Huffman_DecompressX1_Kernel)(cmprBuffer, decBuffer, decSize, maxLitHufBits, litHufCodeDemapX1);
		free(litHufCodeDemapX1);
		return;
	default:
		litHufCodeDemapX2 = (Huffman_DemapX2*)malloc( (Uint32)(4+(1 << hufDemapBitsX2)) * sizeof(Huffman_DemapX2));
		Build_Huffman_DecTableX2(nLits, maxLitHufBits, litHufCodeBits, litHufCodeDemapX2, hufDemapBitsX2);
		HUF_KERNEL(Huffman_Decompress4X2_Kernel)(cmprBuffer, decBuffer, decSize, hufDemapBitsX2, litHufCodeDemapX2);
		free(litHufCodeDemapX2);
		return;
	}
}

Uint32 Huffman_Decompress_Block(const void *source, void* dest, Uint32 destSize, int nLits)
{
	Uint8* srcPtr = (Uint8*)source;
	Uint8* destPtr = (Uint8*)dest;
	if (0 == *srcPtr++) {  /* uncompressed */
		MemWildCopy(destPtr, srcPtr, destPtr + destSize);
		return destSize+1;
	}

	Uint32 comprSize = MemReadLE2(srcPtr);
	srcPtr += 2;
	Bit_Stream bitStream;
	bitStream.nUsedBits = 0;
	bitStream.container = MemReadBE8(srcPtr);
	bitStream.streamPtr = srcPtr;
	Uint8 hufHufCodeBits[MAX_HufWeight + 3], litHufCodeBits[MAX_HufSize];
	Huffman_DemapX1 hufHufCodeDemapX1[1 << MAX_HufHufWt];
	Uint32 maxHufHufCodeBits = Read_Huffman_Header(&bitStream, MAX_HufWeight + 3, MAX_HufHufWt, hufHufCodeBits);

	Build_Huffman_DecTableX1(MAX_HufWeight + 3, maxHufHufCodeBits, hufHufCodeBits, hufHufCodeDemapX1);
	const Uint32 maxHufCodeBits = Read_Huffman_Header_byHuffman(&bitStream, maxHufHufCodeBits, hufHufCodeDemapX1, nLits, litHufCodeBits);

	BITStream_Read_FlushEnd(bitStream);
	srcPtr = bitStream.streamPtr;

	Huffman_Decompress_Block_Body(litHufCodeBits, nLits, maxHufCodeBits, -1, srcPtr, comprSize, destPtr, destSize);
	return (Uint32)(srcPtr + comprSize - (Uint8*)source);
}

/* It returns compressed byte-size for the original destSize bytes of literals */
Uint32 Huffman_Decompress(const void* source, void* dest, Uint32 destSize, int nLits)
{
	Uint32 srcBlkSize;
	Uint8* srcPtr = (Uint8*)source;
	Uint8* destPtr = (Uint8*)dest;
	while (destSize >= HUF_BlockSize) {
		destSize -= HUF_BlockSize;
		srcBlkSize = Huffman_Decompress_Block(srcPtr, destPtr, HUF_BlockSize, nLits);
		srcPtr += srcBlkSize;
		destPtr += HUF_BlockSize;
	}
	if (destSize > 0) {
		srcBlkSize = Huffman_Decompress_Block(srcPtr, destPtr, destSize, nLits);
		srcPtr += srcBlkSize;
	}

	return (Uint32)(srcPtr - (Uint8*)source);
}