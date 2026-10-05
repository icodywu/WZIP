/*
 *  WZIP_S : WZIP for short fixed-size blocks (e.g. 4K/8K storage pages)
 * Copyright (c) 2018-present, Yingquan (Cody) Wu.
 * SPDX-License-Identifier: BSD-2-Clause (see LICENSE)
 *
 *  Block format (all multi-byte header fields little-endian):
 *    byte 0   : bits 0-1 mode (0 stored, 1 fill, 2 Huffman-coded)
 *               bits 2-3 size class (0: 4096, 1: 8192, 2: 16384, 3: explicit)
 *               bit  4   a dictionary is required to decode (Huffman mode only)
 *               bits 5-7 format version, must be 0
 *    [2 bytes]: original size - 1, present only for the explicit size class
 *    stored   : the original bytes
 *    fill     : one byte, repeated to the original size
 *    Huffman  : MSB-first bitstreams; the main stream holds
 *               - a weight table for the code-length weights (Write_Huffman_Header)
 *               - the code-length weights of all tables, coded by that table
 *                 (literals, literal runs, match lengths, offsets)
 *               - the literal count in log2(block size) + 1 bits
 *               - blocks from 8K only: the byte sizes of literal streams C and D (16 bits each),
 *                 the end of this header stream, stream C, stream D, and a new main stream
 *               - its share of the literals, packed as in zstd so the decoder decodes them in
 *                 tight loops and copies runs in bulk
 *               - per sequence: literal run and match length
 *             Stream B, stored byte-reversed so that it ends at the block end, holds the last
 *             share of the literals followed by the offsets. The decoder reads it backward from
 *             the known block end, so only C and D need sizes. Literals are split into halves
 *             (main, B) or, from 8K, quarters (C, D, main, B), decoded in parallel.
 *
 *  The original size comes first, so the decoder allocates exactly and derives the
 *  window schedule before decoding. The match-length symbol determines the offset coding:
 *    symbol 0  : length 2, distance - 1 in S_W2_BITS raw bits
 *    symbol 1  : length 2 at repeat slot 0, no offset field
 *    symbol 2+ : length code - 1 for lengths >= 3; repeat slots and offset ranges follow,
 *                Huffman-coded with one table spanning the block (length 3 is admitted
 *                by the encoder only within min(S_W3_BITS, half the block))
 *  Length-2 matches never change the repeat slots.
 *
 *  Match search, per length class:
 *    length 2  : repeat slot 0, else the hash table, directly indexed by the 2-byte seed, so the
 *                entry is the most recent occurrence of that exact seed, i.e. the nearest match
 *    length 3+ : hash table and hash chain on 3-byte seeds, retaining the whole block;
 *                length 3 is admitted within min(S_W3_BITS, half the block), longer matches
 *                anywhere in the block
 *
 *  Dictionary: the dictionary (its last WZIPS_MAX_DICT bytes) logically precedes the block,
 *  occupying positions -dictSize .. -1; offsets are ordinary distances into this concatenated
 *  history, and a match may run from the dictionary into the block. A prepared dictionary
 *  (WZIPS_CDict) holds the 3-byte hash heads and chain of the dictionary positions, built once
 *  and only read while compressing: a block position whose previous seed occurrence lies in the
 *  dictionary links to that negative position, so one chain walk covers both. The offset table
 *  spans the history, i.e. log2(dictionary + block) bits, up to 16.
 */
#include <stdlib.h>
#include <string.h>
#include "Memry.h"
#include "BitStream_Huffman.h"
#include "WZIP.h"

#define S_MinMatchLen     2
#define S_NumRep          3                     /* repeat-offset slots, used by matches of length >= 3 */
#ifndef S_W2_BITS
#define S_W2_BITS         0                     /* raw offset bits of length-2 matches; 0 disables them (ratio-neutral, faster decoding) */
#endif
#ifndef S_W3_BITS
#define S_W3_BITS         12                    /* window width for length-3 matches, at most half the block */
#endif
#ifndef S_LEN2_COST
#define S_LEN2_COST       9                     /* estimated bits of a length-2 match: length code + S_W2_BITS */
#endif
#ifndef S_REP2_COST
#define S_REP2_COST       6                     /* estimated bits of a length-2 match at repeat slot 0 */
#endif
#define S_Hash2Log        16                    /* direct index of the 2-byte seed */
#define S_Hash3Log        13
#ifndef S_CapLitBits
#define S_CapLitBits      10                    /* longest literal code; the decoder fills 2^this table entries */
#endif
#ifndef S_CapSeqBits
#define S_CapSeqBits      9                     /* longest literal-run, length and offset code */
#endif
#define S_N_Lit           256
/* Alphabet sizes for blocks up to WZIPS_MAX_BLOCK = 2^15 bytes and dictionaries up to WZIPS_MAX_DICT:
   literal run <= 2^15             : value codes 0..31 (msb 15)
   match length <= 2^15 - 1        : value codes 3..30 (msb 14), plus the two length-2 symbols
   distance < block + dictionary <= 2^16, offVal = distance + 2 <= 2^16 + 1 : offset symbols 0..32 */
#define S_N_LitRun        32
#define S_N_MchLen        30                    /* 0: length 2 raw, 1: length 2 at repeat slot 0, 2..29: length code - 1 */
#define S_N_MchOffMax     33                    /* S_N_OffSym(16) */
#define S_NoPos           (-32768)              /* chain end; positions span -WZIPS_MAX_DICT .. WZIPS_MAX_BLOCK - 1 */
#define S_N_WtSym         (MAX_HufWeight + 3)
#define S_ValueDirect     8
#define S_DirectLog       3
#ifndef S_FourStreamLog
#define S_FourStreamLog   13                    /* blocks above 2^(this - 1) bytes, i.e. from 8K, use four literal streams */
#endif
#define S_DecPad          2048                  /* zero padding after the compressed body */

enum { S_MODE_STORED = 0, S_MODE_FILL = 1, S_MODE_HUF = 2 };

/* Values 0..7 have their own symbols; larger ones a range symbol plus raw low bits: four ranges each for msb 3..5,
   two for msb 6..7, one per msb from 8 up (a small alphabet keeps the tables of 4K/8K blocks cheap) */
static const Uint8 S_RangeBase[16]  = { 0, 0, 0, 8, 12, 16, 20, 22, 24, 25, 26, 27, 28, 29, 30, 31 };
static const Uint8 S_RangeShift[16] = { 0, 0, 0, 1, 2, 3, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15 };

typedef struct { Uint32 litRun, mchLen, offVal; } S_Seq;     /* offVal: 0..2 repeat slot, else offset + 2 */
typedef struct { Uint32 len, offVal; } S_Match;
/* window widths of lengths 2, 3 and >= 4 (the whole history: dictionary + block), and of the block */
typedef struct { int w2, w3, wFull, wBlock; } S_Window;

/* A prepared dictionary: its content and the 3-byte hash heads and chain of its positions */
struct WZIPS_CDict_s {
	Sint32 dictSize;
	Sint16 hash3Table[1 << S_Hash3Log];          /* most recent dictionary position (negative) per bucket, or S_NoPos */
	Sint16 chain3Table[WZIPS_MAX_DICT];          /* indexed by position + dictSize: the previous position, or S_NoPos */
	Uint8  content[WZIPS_MAX_DICT + 8];
};

/* Hash-table entries hold base + position + 1 and are valid only if above the current base;
   advancing the base by the block size invalidates a whole table without clearing it. The
   16-bit entries are cleared only when base + block size would exceed 65535. */
struct WZIPS_CCtx_s {
	Uint32 base;
	Uint16 litPrice[256];                        /* order-0 literal cost of the block, in 1/16 bit */
#if S_W2_BITS
	Uint16 hash2Table[1 << S_Hash2Log];          /* only needed for raw length-2 matches */
#endif
	Uint16 hash3Table[1 << S_Hash3Log];
	Sint16 chain3Table[WZIPS_MAX_BLOCK];         /* previous position of the 3-byte seed, negative in the dictionary, or S_NoPos */
	const WZIPS_CDict* dict;                     /* dictionary of the block being compressed, or NULL */
	const Uint8* dictEnd;
	Sint32 dictSize;
	S_Seq  seq[WZIPS_MAX_BLOCK / S_MinMatchLen + 2];
	Uint8  out[WZIPS_MAX_BLOCK * 2 + 4096];
	Uint8  litB[WZIPS_MAX_BLOCK * 2 + 64];       /* backward stream, before it is reversed onto the block end */
	Uint8  litC[WZIPS_MAX_BLOCK + 64];           /* literal streams C and D of four-stream blocks */
	Uint8  litD[WZIPS_MAX_BLOCK + 64];
	Uint8  lits[WZIPS_MAX_BLOCK];                /* all literals of the block, gathered */
};

typedef struct { int chain3Cnt, lazyDepth; } S_Level;
static const S_Level S_LevelTable[10] = {
	{ 4, 0 }, { 4, 0 }, { 8, 1 }, { 16, 1 }, { 32, 1 }, { 64, 2 }, { 128, 2 }, { 256, 2 }, { 512, 2 }, { 4096, 2 } };

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Shared helpers ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

static int S_SizeClass(int size) { return size == 4096 ? 0 : size == 8192 ? 1 : size == 16384 ? 2 : 3; }

/* Window widths of the length classes 2, 3, >=4; the last class covers the whole history */
static S_Window S_Set_Window(int blockSize, int dictSize)
{
	S_Window win;
	win.wBlock = 1;
	while ((1 << win.wBlock) < blockSize) win.wBlock++;
	win.wFull = win.wBlock;
	while ((1 << win.wFull) < blockSize + dictSize) win.wFull++;
	win.w2 = Min(S_W2_BITS, win.wFull);
	win.w3 = Min(S_W3_BITS, win.wFull - 1);   /* 11 bits for 4K blocks, 12 bits from 8K up */
	return win;
}

/* Offset symbols over a w-bit window, i.e. distances below 2^w: repeat slots 0..2 and exact
   offVal 3, then ranges of offVal = distance + 2 <= 2^w + 1, whose msb w gives symbol 2w at most */
ForceInlineTemplate Uint32 S_N_OffSym(int offWidth) { return offWidth <= 1 ? 4 : 2 * (Uint32)offWidth + 1; }

ForceInlineTemplate void S_Value_Code(Uint32 value, Uint32* sym, Uint32* nExtra, Uint32* extra)
{
	if (value < S_ValueDirect) { *sym = value; *nExtra = 0; *extra = 0; return; }
	Uint32 msb = High_Bit32(value), shift = S_RangeShift[msb];
	*sym = S_RangeBase[msb] + ((value - (1u << msb)) >> shift);
	*nExtra = shift;
	*extra = value & BitMask[shift];
}

/* Match-length symbol: 0 length 2 raw, 1 length 2 at repeat slot 0, else length code - 1 */
ForceInlineTemplate void S_Length_Code(Uint32 mchLen, Uint32 offVal, Uint32* sym, Uint32* nExtra, Uint32* extra)
{
	if (mchLen == 2) { *sym = offVal == 0; *nExtra = 0; *extra = 0; return; }
	S_Value_Code(mchLen, sym, nExtra, extra);
	*sym -= 1;
}

ForceInlineTemplate void S_Offset_Code(Uint32 offVal, Uint32* sym, Uint32* nExtra, Uint32* extra)
{
	if (offVal < 4) { *sym = offVal; *nExtra = 0; *extra = 0; return; }
	Uint32 msb = High_Bit32(offVal);
	*sym = 2 * msb + ((offVal >> (msb - 1)) & 1);
	*nExtra = msb - 1;
	*extra = offVal & BitMask[msb - 1];
}

/* Repeat-offset update shared by compressor and decompressor; returns the actual offset */
ForceInlineTemplate Uint32 S_Update_Rep(Uint32* rep, Uint32 offVal)
{
	Uint32 off;
	if (offVal >= S_NumRep) {
		off = offVal - (S_NumRep - 1);
		rep[2] = rep[1]; rep[1] = rep[0]; rep[0] = off;
	} else if (offVal == 1) {
		off = rep[1]; rep[1] = rep[0]; rep[0] = off;
	} else if (offVal == 2) {
		off = rep[2]; rep[2] = rep[1]; rep[1] = rep[0]; rep[0] = off;
	} else off = rep[0];
	return off;
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Match finder ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

ForceInlineTemplate Uint32 S_Hash3(const Uint8* p) { return ((MemReadLE4(p) << 8) * HashPrime4) >> (32 - S_Hash3Log); }

ForceInlineTemplate Uint32 S_Match_Count(const Uint8* srcPtr, const Uint8* matchPtr, const Uint8* const srcEnd)
{
	const Uint8* const srcStart = srcPtr;
	while (srcPtr + REG_SIZE <= srcEnd) {
		reg_t diff = MemReadARCH(srcPtr) ^ MemReadARCH(matchPtr);
		if (diff) return (Uint32)(srcPtr - srcStart) + N_ZeroBytes(diff);
		srcPtr += REG_SIZE;
		matchPtr += REG_SIZE;
	}
	while (srcPtr < srcEnd && *srcPtr == *matchPtr) { srcPtr++; matchPtr++; }
	return (Uint32)(srcPtr - srcStart);
}

/* Match length at history position idx (negative in the dictionary): a dictionary match continues
   into the block, as in the concatenated history. hasDict is a compile-time constant, so the
   no-dictionary instantiation reduces to S_Match_Count. */
ForceInlineTemplate Uint32 S_Match_Count_Hist(const int hasDict, const Uint8* srcPtr, const Uint8* const srcEnd, Sint32 idx,
	const Uint8* source, const Uint8* dictEnd)
{
	if (!hasDict || idx >= 0) return S_Match_Count(srcPtr, source + idx, srcEnd);
	const Uint32 room = (Uint32)(-idx);
	const Uint8* const dictLimit = (Uint32)(srcEnd - srcPtr) > room ? srcPtr + room : srcEnd;
	const Uint32 len = S_Match_Count(srcPtr, dictEnd + idx, dictLimit);
	if (len < room || srcPtr + len == srcEnd) return len;
	return len + S_Match_Count(srcPtr + len, source, srcEnd);
}

ForceInlineTemplate void S_Insert(const int hasDict, WZIPS_CCtx* cctx, const Uint8* source, Uint32 pos)
{
	const Uint32 base = cctx->base, h3 = S_Hash3(source + pos);
	const Uint32 prev = cctx->hash3Table[h3];
	cctx->chain3Table[pos] = (Sint16)(prev > base ? (Sint32)(prev - base - 1) : hasDict ? cctx->dict->hash3Table[h3] : S_NoPos);
	cctx->hash3Table[h3] = (Uint16)(base + pos + 1);
#if S_W2_BITS
	cctx->hash2Table[MemReadLE2(source + pos)] = (Uint16)(base + pos + 1);
#endif
}

/* log2(x) in 1/16 bit, linear between powers of two (error below 0.09 bit) */
ForceInlineTemplate Uint32 S_Log2x16(Uint32 x) { Uint32 msb = High_Bit32(x); return 16 * msb + (((x << 4) >> msb) & 15); }

/* A length-2 match pays only if it costs less than its two literals, a(2) + w(2) < 2b */
ForceInlineTemplate int S_Len2_Pays(const WZIPS_CCtx* cctx, const Uint8* srcPtr, Uint32 costBits)
{
	return cctx->litPrice[srcPtr[0]] + cctx->litPrice[srcPtr[1]] > 16 * costBits;
}

/* Approximate saving of a match in quarter-bits relative to its literals, as in zstd's lazy parser */
ForceInlineTemplate int S_Gain(S_Match m) { return m.len ? (int)(4 * m.len) - (int)High_Bit32(m.offVal + 1) : 0; }

/* Searches position pos, all earlier positions being inserted. Repeat offsets serve lengths >= 3 at any
   distance, and length 2 at slot 0; explicit offsets must lie within the window of their length class.
   The 3-byte chain keeps the longest admissible match (nearest on ties); length 2 is consulted only when
   that fails. hasDict is a compile-time constant: without a dictionary, all positions are non-negative. */
ForceInlineTemplate S_Match S_Search(const int hasDict, WZIPS_CCtx* cctx, const Uint8* source, const Uint8* srcEnd, Uint32 pos,
	const Uint32* rep, S_Window win, const S_Level* lv)
{
	const Uint8* const srcPtr = source + pos;
	const Uint32 base = cctx->base;
	const Uint8* const dictEnd = hasDict ? cctx->dictEnd : NULL;
	const Sint32 dictSize = hasDict ? cctx->dictSize : 0;
	const Uint32 seed3 = MemReadLE4(srcPtr) & 0xFFFFFF;
	const Uint32 window3 = 1u << win.w3, windowFull = 1u << win.wFull;
	S_Match best = { 0, 0 }, cand;
	int k, searchCnt;

	for (k = 0; k < S_NumRep; k++) {
		if (rep[k] > pos + (Uint32)dictSize) continue;
		const Sint32 idx = (Sint32)pos - (Sint32)rep[k];
		/* quick seed test, unless the 3 bytes straddle the dictionary end */
		if ((!hasDict || idx >= 0 || idx <= -3) &&
			(MemReadLE4(!hasDict || idx >= 0 ? source + idx : dictEnd + idx) & 0xFFFFFF) != seed3) continue;
		cand.len = S_Match_Count_Hist(hasDict, srcPtr, srcEnd, idx, source, dictEnd);
		cand.offVal = (Uint32)k;
		if (cand.len >= 3 && S_Gain(cand) > S_Gain(best)) best = cand;
	}

	const Uint32 h3 = S_Hash3(srcPtr), entry = cctx->hash3Table[h3];
	Sint32 matchIdx = entry > base ? (Sint32)(entry - base - 1) : hasDict ? cctx->dict->hash3Table[h3] : S_NoPos;
	for (searchCnt = lv->chain3Cnt; matchIdx != S_NoPos && searchCnt; searchCnt--) {
		const Uint32 dist = (Uint32)((Sint32)pos - matchIdx);
		if (dist >= windowFull) break;
		/* inserted dictionary positions are at most -4, so their 4-byte seed lies in the dictionary */
		if ((MemReadLE4(!hasDict || matchIdx >= 0 ? source + matchIdx : dictEnd + matchIdx) & 0xFFFFFF) == seed3) {
			cand.len = 3 + S_Match_Count_Hist(hasDict, srcPtr + 3, srcEnd, matchIdx + 3, source, dictEnd);
			cand.offVal = dist + (S_NumRep - 1);
			if ((cand.len >= 4 || dist < window3) && S_Gain(cand) > S_Gain(best)) {
				best = cand;
				if (srcPtr + best.len == srcEnd) break;       /* no later candidate can be longer or nearer */
			}
		}
		matchIdx = !hasDict || matchIdx >= 0 ? cctx->chain3Table[matchIdx] : cctx->dict->chain3Table[matchIdx + dictSize];
	}

	if (best.len >= 3) return best;

	/* length 2: repeat slot 0 needs no offset field, so it is preferred; it is not used when the two
	   bytes would straddle the dictionary end (idx0 == -1), which occurs at most once per block */
	const Sint32 idx0 = (Sint32)pos - (Sint32)rep[0];
	if (rep[0] <= pos + (Uint32)dictSize && (!hasDict || idx0 != -1) &&
		MemReadLE2(!hasDict || idx0 >= 0 ? source + idx0 : dictEnd + idx0) == MemReadLE2(srcPtr)) {
		if (S_Len2_Pays(cctx, srcPtr, S_REP2_COST)) { best.len = 2; best.offVal = 0; }
		return best;
	}
#if S_W2_BITS
	/* else the 2-byte seed at the nearest block distance; a longer match there was already seen by the 3-byte chain */
	const Uint32 entry2 = cctx->hash2Table[MemReadLE2(srcPtr)];
	if (entry2 > base) {
		const Uint32 dist = pos - (entry2 - base - 1);
		if (dist <= (1u << win.w2) && S_Len2_Pays(cctx, srcPtr, S_LEN2_COST)) {
			best.len = 2;
			best.offVal = dist + (S_NumRep - 1);
		}
	}
#else
	(void)win;
#endif
	return best;
}

/* Lazy parse into sequences; returns the number of sequences including the terminal literal run.
   Instantiated with and without a dictionary. */
ForceInlineTemplate Uint32 S_Parse_Body(const int hasDict, WZIPS_CCtx* cctx, const Uint8* source, Uint32 srcSize, S_Window win, const S_Level* lv)
{
	const Uint8* const srcEnd = source + srcSize;
	const Uint32 lastSearch = srcSize >= 4 ? srcSize - 4 : 0;   /* seeds are read as 4 bytes */
	Uint32 rep[S_NumRep] = { 1, 4, 8 };
	Uint32 pos = 0, anchor = 0, nextInsert = 0, nSeq = 0;

	if (cctx->base + srcSize > 0xFFFF) {                  /* entries up to base + srcSize must fit in 16 bits */
#if S_W2_BITS
		memset(cctx->hash2Table, 0, sizeof(cctx->hash2Table));
#endif
		memset(cctx->hash3Table, 0, sizeof(cctx->hash3Table));
		cctx->base = 0;
	}

	Uint32 i, histo[256] = { 0 };
	for (i = 0; i < srcSize; i++) histo[source[i]]++;
	for (i = 0; i < 256; i++)
		cctx->litPrice[i] = (Uint16)(S_Log2x16(srcSize) - (histo[i] ? S_Log2x16(histo[i]) : 0));

	while (srcSize >= 4 && pos <= lastSearch) {
		while (nextInsert < pos) S_Insert(hasDict, cctx, source, nextInsert++);
		S_Match m = S_Search(hasDict, cctx, source, srcEnd, pos, rep, win, lv);
		if (m.len < S_MinMatchLen) { pos++; continue; }

		for (int depth = 0; depth < lv->lazyDepth && pos + 1 <= lastSearch; depth++) {
			while (nextInsert < pos + 1) S_Insert(hasDict, cctx, source, nextInsert++);
			S_Match m1 = S_Search(hasDict, cctx, source, srcEnd, pos + 1, rep, win, lv);
			if (m1.len >= S_MinMatchLen && S_Gain(m1) > S_Gain(m) + (depth ? 7 : 4)) {
				m = m1;
				pos++;
			}
			else break;
		}

		S_Seq* s = cctx->seq + nSeq++;
		s->litRun = pos - anchor;
		s->mchLen = m.len;
		if (m.len >= 3) {
			/* an explicit offset equal to a repeat slot is coded as that slot */
			if (m.offVal >= S_NumRep) {
				const Uint32 off = m.offVal - (S_NumRep - 1);
				for (int k = 0; k < S_NumRep; k++) if (rep[k] == off) { m.offVal = (Uint32)k; break; }
			}
			S_Update_Rep(rep, m.offVal);
		}
		s->offVal = m.offVal;
		pos += m.len;
		anchor = pos;
	}
	cctx->seq[nSeq].litRun = srcSize - anchor;
	cctx->seq[nSeq].mchLen = 0;
	cctx->base += srcSize;
	return nSeq + 1;
}

static Uint32 S_Parse(WZIPS_CCtx* cctx, const Uint8* source, Uint32 srcSize, S_Window win, const S_Level* lv)
{
	return cctx->dict ? S_Parse_Body(1, cctx, source, srcSize, win, lv) : S_Parse_Body(0, cctx, source, srcSize, win, lv);
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Entropy coding ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

typedef struct {
	Huffman_Str lit[S_N_Lit], litRun[S_N_LitRun], mchLen[S_N_MchLen], mchOff[S_N_MchOffMax];
} S_Freq;

typedef struct {
	HufCode_Str lit[S_N_Lit], litRun[S_N_LitRun], mchLen[S_N_MchLen], mchOff[S_N_MchOffMax];
} S_Code;

#define S_WRITE(bs, sym, nb)   { if (nb) BITStream_Write(bs, (sym), (nb)); }

/* Writes n literals as a complete byte-aligned stream; returns its size in bytes */
static Uint32 S_Encode_Lits(const Uint8* lits, Uint32 n, const HufCode_Str* litCode, Uint8* dest)
{
	Bit_Stream bitStream = { 0, 0, dest };
	for (Uint32 i = 0; i < n; i++) {
		S_WRITE(bitStream, litCode[lits[i]].code, litCode[lits[i]].nbits);
		if ((i & 3) == 3) BITStream_Write_Flush(bitStream);
	}
	BITStream_Write_FlushEnd(bitStream);
	return (Uint32)(bitStream.streamPtr - dest);
}

static Uint32 S_Encode(WZIPS_CCtx* cctx, const Uint8* source, Uint32 nSeq, S_Window win, Uint8* dest)
{
	static const S_Freq freqZero;
	S_Freq freq = freqZero;
	S_Code code;
	Uint32 i, n, sym, nExtra, extra;
	const Uint32 nOffSym = S_N_OffSym(win.wFull);
	const Uint8* litPtr = source;

	for (n = 0; n < nSeq; n++) {
		const S_Seq* s = cctx->seq + n;
		for (i = 0; i < s->litRun; i++) freq.lit[litPtr[i]].freq++;
		litPtr += s->litRun + s->mchLen;
		S_Value_Code(s->litRun, &sym, &nExtra, &extra);
		freq.litRun[sym].freq++;
		if (n + 1 == nSeq) break;
		S_Length_Code(s->mchLen, s->offVal, &sym, &nExtra, &extra);
		freq.mchLen[sym].freq++;
		if (s->mchLen == 2) continue;
		S_Offset_Code(s->offVal, &sym, &nExtra, &extra);
		freq.mchOff[sym].freq++;
	}

	Build_Huffman_Table(freq.lit, S_N_Lit, S_CapLitBits, code.lit);
	Build_Huffman_Table(freq.litRun, S_N_LitRun, S_CapSeqBits, code.litRun);
	Build_Huffman_Table(freq.mchLen, S_N_MchLen, S_CapSeqBits, code.mchLen);
	Build_Huffman_Table(freq.mchOff, nOffSym, S_CapSeqBits, code.mchOff);

	/* one weight table serves the code lengths of every table */
	Huffman_Str wtFreq[S_N_WtSym] = { 0 };
	HufCode_Str wtCode[S_N_WtSym];
	Uint8 wtSeq[2 * (S_N_Lit + S_N_LitRun + S_N_MchLen + S_N_MchOffMax)];
	Uint32 wtSeqSize = 0;
	wtSeqSize += Count_Huffman_Weight_Frequency(code.lit, S_N_Lit, wtFreq, wtSeq + wtSeqSize);
	wtSeqSize += Count_Huffman_Weight_Frequency(code.litRun, S_N_LitRun, wtFreq, wtSeq + wtSeqSize);
	wtSeqSize += Count_Huffman_Weight_Frequency(code.mchLen, S_N_MchLen, wtFreq, wtSeq + wtSeqSize);
	wtSeqSize += Count_Huffman_Weight_Frequency(code.mchOff, nOffSym, wtFreq, wtSeq + wtSeqSize);
	Build_Huffman_Table(wtFreq, S_N_WtSym, MAX_HufHufWt, wtCode);

	Bit_Stream bitStream = { 0, 0, dest };
	Write_Huffman_Header(&bitStream, S_N_WtSym, MAX_HufHufWt, wtCode);
	Write_Huffman_Header_byHuffman(&bitStream, wtCode, wtSeq, (int)wtSeqSize);

	/* all literals, packed as in zstd and split into streams decoded in parallel:
	   - two streams: the first half in this main stream, the second half in stream B, which is
	     stored byte-reversed at the end of the block and read backward from the known block end
	   - four streams (blocks from 8K): quarters in streams C and D, whose byte sizes close this
	     header and which follow it, then quarter A in the main stream and the last quarter in B */
	Uint32 nLits = 0;
	for (n = 0, litPtr = source; n < nSeq; n++) {
		memcpy(cctx->lits + nLits, litPtr, cctx->seq[n].litRun);
		nLits += cctx->seq[n].litRun;
		litPtr += cctx->seq[n].litRun + cctx->seq[n].mchLen;
	}
	S_WRITE(bitStream, nLits, (Uint32)win.wBlock + 1);
	BITStream_Write_Flush(bitStream);
	Uint32 startA = 0, nLitsA = (nLits + 1) / 2;
	if (win.wBlock >= S_FourStreamLog) {
		const Uint32 q = nLits >> 2;
		const Uint32 sizeC = S_Encode_Lits(cctx->lits, q, code.lit, cctx->litC);
		const Uint32 sizeD = S_Encode_Lits(cctx->lits + q, q, code.lit, cctx->litD);
		S_WRITE(bitStream, sizeC, 16);
		S_WRITE(bitStream, sizeD, 16);
		BITStream_Write_FlushEnd(bitStream);
		memcpy(bitStream.streamPtr, cctx->litC, sizeC);
		memcpy(bitStream.streamPtr + sizeC, cctx->litD, sizeD);
		bitStream.streamPtr += sizeC + sizeD;
		bitStream.container = 0;
		bitStream.nUsedBits = 0;
		startA = 2 * q;
		nLitsA = q;
	}
	Bit_Stream bitStreamB = { 0, 0, cctx->litB };
	for (i = 0; i < nLitsA; i++) {
		S_WRITE(bitStream, code.lit[cctx->lits[startA + i]].code, code.lit[cctx->lits[startA + i]].nbits);
		if ((i & 3) == 3) BITStream_Write_Flush(bitStream);
	}
	for (i = startA + nLitsA; i < nLits; i++) {
		S_WRITE(bitStreamB, code.lit[cctx->lits[i]].code, code.lit[cctx->lits[i]].nbits);
		if (((i - startA - nLitsA) & 3) == 3) BITStream_Write_Flush(bitStreamB);
	}
	BITStream_Write_Flush(bitStream);
	BITStream_Write_Flush(bitStreamB);

	/* sequences: literal run and match length in the main stream, offsets in stream B,
	   so that the decoder follows two independent bit positions */
	for (n = 0; n < nSeq; n++) {
		const S_Seq* s = cctx->seq + n;
		S_Value_Code(s->litRun, &sym, &nExtra, &extra);
		S_WRITE(bitStream, code.litRun[sym].code, code.litRun[sym].nbits);
		S_WRITE(bitStream, extra, nExtra);
		if (n + 1 == nSeq) break;

		Uint32 lenSym, lenExtra, lenNExtra;
		S_Length_Code(s->mchLen, s->offVal, &lenSym, &lenNExtra, &lenExtra);
		S_WRITE(bitStream, code.mchLen[lenSym].code, code.mchLen[lenSym].nbits);
		S_WRITE(bitStream, lenExtra, lenNExtra);
		BITStream_Write_Flush(bitStream);
		if (s->mchLen == 2) {
			if (s->offVal) S_WRITE(bitStreamB, s->offVal - S_NumRep, (Uint32)win.w2);      /* distance - 1 */
		}
		else {
			S_Offset_Code(s->offVal, &sym, &nExtra, &extra);
			S_WRITE(bitStreamB, code.mchOff[sym].code, code.mchOff[sym].nbits);
			S_WRITE(bitStreamB, extra, nExtra);
		}
		BITStream_Write_Flush(bitStreamB);
	}
	BITStream_Write_FlushEnd(bitStream);
	BITStream_Write_FlushEnd(bitStreamB);
	const Uint32 sizeB = (Uint32)(bitStreamB.streamPtr - cctx->litB);
	for (i = 0; i < sizeB; i++) bitStream.streamPtr[i] = cctx->litB[sizeB - 1 - i];
	return (Uint32)(bitStream.streamPtr - dest) + sizeB;
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Compression API ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

WZIPS_CCtx* WZIPS_createCCtx(void) { return (WZIPS_CCtx*)calloc(1, sizeof(WZIPS_CCtx)); }

void WZIPS_freeCCtx(WZIPS_CCtx* cctx) { free(cctx); }

WZIPS_CDict* WZIPS_createCDict(const void* dict, int dictSize)
{
	if (dict == NULL || dictSize < 1) return NULL;
	WZIPS_CDict* cdict = (WZIPS_CDict*)calloc(1, sizeof(WZIPS_CDict));
	if (cdict == NULL) return NULL;
	const Sint32 size = Min(dictSize, WZIPS_MAX_DICT);                     /* the last bytes are the nearest history */
	cdict->dictSize = size;
	memcpy(cdict->content, (const Uint8*)dict + dictSize - size, size);
	for (Uint32 h = 0; h < (1u << S_Hash3Log); h++) cdict->hash3Table[h] = S_NoPos;
	/* positions -size .. -4: their 4-byte seed read stays in the dictionary */
	for (Sint32 p = -size; p <= -4; p++) {
		const Uint32 h3 = S_Hash3(cdict->content + size + p);
		cdict->chain3Table[p + size] = cdict->hash3Table[h3];
		cdict->hash3Table[h3] = (Sint16)p;
	}
	return cdict;
}

void WZIPS_freeCDict(WZIPS_CDict* cdict) { free(cdict); }

static int S_Write_Header(Uint8* dest, int mode, int srcSize, int useDict)
{
	const int sizeClass = S_SizeClass(srcSize);
	dest[0] = (Uint8)(mode | sizeClass << 2 | (useDict ? 16 : 0));
	if (sizeClass != 3) return 1;
	MemWriteLE2(dest + 1, (Uint16)(srcSize - 1));
	return 3;
}

int WZIPS_compress(WZIPS_CCtx* cctx, const void* src, int srcSize, void* dst, int dstCap, int level)
{
	return WZIPS_compress_usingCDict(cctx, NULL, src, srcSize, dst, dstCap, level);
}

int WZIPS_compress_usingCDict(WZIPS_CCtx* cctx, const WZIPS_CDict* cdict, const void* src, int srcSize, void* dst, int dstCap, int level)
{
	const Uint8* const source = (const Uint8*)src;
	Uint8* const dest = (Uint8*)dst;
	int i;

	if (cctx == NULL || srcSize < 1 || srcSize > WZIPS_MAX_BLOCK || dstCap < WZIPS_COMPRESSBOUND(srcSize)) return 0;
	level = level < 1 ? 1 : level > 9 ? 9 : level;

	for (i = 1; i < srcSize && source[i] == source[0]; i++);
	if (i == srcSize) {
		int hdrSize = S_Write_Header(dest, S_MODE_FILL, srcSize, 0);
		dest[hdrSize] = source[0];
		return hdrSize + 1;
	}

	cctx->dict = cdict;
	cctx->dictSize = cdict ? cdict->dictSize : 0;
	cctx->dictEnd = cdict ? cdict->content + cdict->dictSize : NULL;
	const S_Window win = S_Set_Window(srcSize, cctx->dictSize);
	Uint32 nSeq = S_Parse(cctx, source, (Uint32)srcSize, win, S_LevelTable + level);
	int hdrSize = S_Write_Header(cctx->out, S_MODE_HUF, srcSize, cdict != NULL);
	int cmprSize = hdrSize + (int)S_Encode(cctx, source, nSeq, win, cctx->out + hdrSize);

	if (cmprSize >= srcSize + hdrSize) {
		hdrSize = S_Write_Header(dest, S_MODE_STORED, srcSize, 0);
		memcpy(dest + hdrSize, source, srcSize);
		return hdrSize + srcSize;
	}
	memcpy(dest, cctx->out, cmprSize);
	return cmprSize;
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Decompression ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

static int S_Read_Header(const Uint8* source, int srcSize, int* mode, int* hdrSize, int* useDict)
{
	static const int classSize[3] = { 4096, 8192, 16384 };
	if (srcSize < 1 || (source[0] >> 5)) return -1;
	*mode = source[0] & 3;
	*useDict = (source[0] >> 4) & 1;
	const int sizeClass = (source[0] >> 2) & 3;
	if (*mode == 3 || (*useDict && *mode != S_MODE_HUF)) return -1;
	if (sizeClass != 3) { *hdrSize = 1; return classSize[sizeClass]; }
	if (srcSize < 3) return -1;
	*hdrSize = 3;
	return 1 + MemReadLE2(source + 1);
}

int WZIPS_getDecompressedSize(const void* src, int srcSize)
{
	int mode, hdrSize, useDict;
	return S_Read_Header((const Uint8*)src, srcSize, &mode, &hdrSize, &useDict);
}

/* Validates code lengths and returns the maximum length (0 for an empty table), or -1.
   Build_Huffman_Table emits a complete code, or a single symbol of length 1, and
   Build_Huffman_DecTableX1 relies on that, so any other code is rejected. */
static int S_Check_Code(const Uint8* hufCodeBits, Uint32 nSym, Uint32 capBits)
{
	Uint32 i, kraft = 0, maxBits = 0, nEffSym = 0;
	for (i = 0; i < nSym; i++) {
		if (hufCodeBits[i] > capBits) return -1;
		if (hufCodeBits[i]) { kraft += 1u << (capBits - hufCodeBits[i]); maxBits = Max(maxBits, hufCodeBits[i]); nEffSym++; }
	}
	if (nEffSym == 0) return 0;
	if (nEffSym == 1) return maxBits == 1 ? 1 : -1;
	return kraft == (1u << capBits) ? (int)maxBits : -1;
}

/* Reads one table's code lengths and builds its single-lookup demapper */
static int S_Read_Table(Bit_Stream* bitStream, Uint32 maxWtBits, Huffman_DemapX1* wtDemap, Uint32 nSym, Uint32 capBits,
	Uint8* hufCodeBits, Huffman_DemapX1* demap)
{
	Read_Huffman_Header_byHuffman(bitStream, maxWtBits, wtDemap, nSym, hufCodeBits);
	const int maxBits = S_Check_Code(hufCodeBits, nSym, capBits);
	if (maxBits > 0) Build_Huffman_DecTableX1(nSym, (Uint32)maxBits, hufCodeBits, demap);
	return maxBits;
}

#define S_READ_BITS(bs, nb, v)   { if (nb) { BITStream_Read(bs, nb, v); } else v = 0; }

/* Refill of a stream stored byte-reversed and read backward: the byte at the lower address comes
   later in the stream, so a little-endian load puts the next stream byte in the top bits */
#define S_READ_FLUSH_BACK(bs)    { bs.streamPtr -= bs.nUsedBits >> 3; bs.container = MemReadLE8(bs.streamPtr); bs.nUsedBits &= 7; }

int WZIPS_decompress(const void* src, int srcSize, void* dst, int dstCap)
{
	return WZIPS_decompress_usingDict(src, srcSize, dst, dstCap, NULL, 0);
}

ForceInlineTemplate int S_Decompress_Body(const void* src, int srcSize, void* dst, int dstCap, const void* dict, int dictSize)
{
	const Uint8* const source = (const Uint8*)src;
	Uint8* const dest = (Uint8*)dst;
	int mode, hdrSize, useDict;
	Uint32 i;
	const int decSize = S_Read_Header(source, srcSize, &mode, &hdrSize, &useDict);
	if (decSize < 0 || dstCap < decSize) return -1;
	if (useDict && (dict == NULL || dictSize < 1)) return -1;
	/* as in WZIPS_createCDict, only the last WZIPS_MAX_DICT bytes form the history */
	const Sint32 histDict = useDict ? Min(dictSize, WZIPS_MAX_DICT) : 0;
	const Uint8* const dictEnd = useDict ? (const Uint8*)dict + dictSize : NULL;

	if (mode == S_MODE_STORED) {
		if (srcSize != hdrSize + decSize) return -1;
		memcpy(dest, source + hdrSize, decSize);
		return decSize;
	}
	if (mode == S_MODE_FILL) {
		if (srcSize != hdrSize + 1) return -1;
		memset(dest, source[hdrSize], decSize);
		return decSize;
	}

	/* copy the body into a zero-padded buffer so that 8-byte refills never leave it, forward or backward */
	const int bodySize = srcSize - hdrSize;
	if (bodySize > WZIPS_MAX_BLOCK + 16) return -1;
	Uint8 bodyBuf[16 + WZIPS_MAX_BLOCK + 16 + S_DecPad];
	Uint8* const body = bodyBuf + 16;
	memset(bodyBuf, 0, 16);
	memcpy(body, source + hdrSize, bodySize);
	memset(body + bodySize, 0, S_DecPad);
	const Uint8* const bodyEnd = body + bodySize;

	Bit_Stream bitStream;
	bitStream.nUsedBits = 0;
	bitStream.streamPtr = body;
	bitStream.container = MemReadBE8(body);

	/* code-length tables; each array has slack for a malformed final repeat run */
	Uint8 wtBits[S_N_WtSym + 288];
	Huffman_DemapX1 wtDemap[1 << MAX_HufHufWt];
	int wtMax;
	Read_Huffman_Header(&bitStream, S_N_WtSym, MAX_HufHufWt, wtBits);
	if ((wtMax = S_Check_Code(wtBits, S_N_WtSym, MAX_HufHufWt)) <= 0) return -1;
	Build_Huffman_DecTableX1(S_N_WtSym, (Uint32)wtMax, wtBits, wtDemap);

	Uint8 litBits[S_N_Lit + 336], litRunBits[S_N_LitRun + 336], mchLenBits[S_N_MchLen + 336], mchOffBits[S_N_MchOffMax + 336];
	Huffman_DemapX1 litDemap[1 << S_CapLitBits], litRunDemap[1 << S_CapSeqBits], mchLenDemap[1 << S_CapSeqBits];
	Huffman_DemapX1 mchOffDemap[1 << S_CapSeqBits];
	int litMax, litRunMax, mchLenMax, mchOffMax;

	const S_Window win = S_Set_Window(decSize, histDict);
	if ((litMax = S_Read_Table(&bitStream, wtMax, wtDemap, S_N_Lit, S_CapLitBits, litBits, litDemap)) < 0) return -1;
	if ((litRunMax = S_Read_Table(&bitStream, wtMax, wtDemap, S_N_LitRun, S_CapSeqBits, litRunBits, litRunDemap)) <= 0) return -1;
	if ((mchLenMax = S_Read_Table(&bitStream, wtMax, wtDemap, S_N_MchLen, S_CapSeqBits, mchLenBits, mchLenDemap)) < 0) return -1;
	if ((mchOffMax = S_Read_Table(&bitStream, wtMax, wtDemap, S_N_OffSym(win.wFull), S_CapSeqBits, mchOffBits, mchOffDemap)) < 0) return -1;

	/* range symbol -> (base value, extra bits) */
	Uint32 valueBase[S_N_LitRun], valueBits[S_N_LitRun];
	for (i = 0; i < S_ValueDirect; i++) { valueBase[i] = i; valueBits[i] = 0; }
	for (i = S_DirectLog; i < 16; i++) {
		Uint32 j, nCodes = 1u << (i - S_RangeShift[i]);
		for (j = 0; j < nCodes; j++) {
			valueBase[S_RangeBase[i] + j] = (1u << i) + (j << S_RangeShift[i]);
			valueBits[S_RangeBase[i] + j] = S_RangeShift[i];
		}
	}

	const Uint32 remLit = 64 - (Uint32)litMax, remLitRun = 64 - (Uint32)litRunMax, remMchLen = 64 - (Uint32)mchLenMax;
	const Uint32 remMchOff = 64 - (Uint32)mchOffMax;
	Uint32 rep[S_NumRep] = { 1, 4, 8 };
	Uint8* destPtr = dest;
	Uint8* const destEnd = dest + decSize;

	/* all literals first, into a buffer with slack for 16-byte copies, from streams decoded in parallel:
	   two streams: the first half from the main stream, the second half from stream B, read backward
	   from the block end; four streams: quarters from C and D, which follow the header, then from
	   the main stream (quarter A, after D) and from stream B */
	Uint32 nLits;
	BITStream_Read(bitStream, (Uint32)win.wBlock + 1, nLits);
	if (nLits > (Uint32)decSize || (nLits && !litMax)) return -1;
	Uint8 litBuf[WZIPS_MAX_BLOCK + 32];
	Uint8* const litBufEnd = litBuf + nLits;
	Uint8* litPtr = litBuf;
	Uint8* litEndA = litBuf + (nLits + 1) / 2;
	Uint8* litPtrB = litEndA;
	Bit_Stream bitStreamB;
	bitStreamB.nUsedBits = 0;
	bitStreamB.streamPtr = (Uint8*)bodyEnd - 8;
	bitStreamB.container = MemReadLE8(bitStreamB.streamPtr);

	if (win.wBlock >= S_FourStreamLog) {
		const Uint32 q = nLits >> 2;
		Uint32 sizeC, sizeD;
		BITStream_Read(bitStream, 16, sizeC);
		BITStream_Read(bitStream, 16, sizeD);
		BITStream_Read_FlushEnd(bitStream);
		Uint8* const startC = bitStream.streamPtr;
		Uint8* const startD = startC + sizeC;
		Uint8* const startA = startD + sizeD;
		if (startA > bodyEnd) return -1;
		Bit_Stream bitStreamC, bitStreamD;
		bitStreamC.nUsedBits = bitStreamD.nUsedBits = bitStream.nUsedBits = 0;
		bitStreamC.streamPtr = startC; bitStreamC.container = MemReadBE8(startC);
		bitStreamD.streamPtr = startD; bitStreamD.container = MemReadBE8(startD);
		bitStream.streamPtr = startA;  bitStream.container = MemReadBE8(startA);
		Uint8* litPtrC = litBuf;
		Uint8* litPtrD = litBuf + q;
		Uint8* const litEndC = litPtrD;
		Uint8* const litEndD = litBuf + 2 * q;
		litPtr = litEndD;
		litEndA = litBuf + 3 * q;
		litPtrB = litEndA;
		while (litPtrC + 4 <= litEndC) {                    /* D and A hold as many, B at least as many */
			BITStream_Read_Flush(bitStreamC);
			BITStream_Read_Flush(bitStreamD);
			BITStream_Read_Flush(bitStream);
			S_READ_FLUSH_BACK(bitStreamB);
			if (bitStreamC.streamPtr > bodyEnd + 8 || bitStreamD.streamPtr > bodyEnd + 8 ||
				bitStream.streamPtr > bodyEnd + 8 || bitStreamB.streamPtr < body - 8) return -1;
			for (int k = 0; k < 4; k++) {
				BITStream_Read_ExtHufX1(bitStreamC, remLit, litDemap, litPtrC[k]);
				BITStream_Read_ExtHufX1(bitStreamD, remLit, litDemap, litPtrD[k]);
				BITStream_Read_ExtHufX1(bitStream, remLit, litDemap, litPtr[k]);
				BITStream_Read_ExtHufX1(bitStreamB, remLit, litDemap, litPtrB[k]);
			}
			litPtrC += 4; litPtrD += 4; litPtr += 4; litPtrB += 4;
		}
		BITStream_Read_Flush(bitStreamC);
		BITStream_Read_Flush(bitStreamD);
		while (litPtrC < litEndC) { BITStream_Read_ExtHufX1(bitStreamC, remLit, litDemap, *litPtrC); litPtrC++; }
		while (litPtrD < litEndD) { BITStream_Read_ExtHufX1(bitStreamD, remLit, litDemap, *litPtrD); litPtrD++; }
		/* C and D must end exactly at their stated sizes; quarters A and B finish below */
		if (bitStreamC.streamPtr + ((bitStreamC.nUsedBits + 7) >> 3) != startD ||
			bitStreamD.streamPtr + ((bitStreamD.nUsedBits + 7) >> 3) != startA) return -1;
	}
	while (litPtr + 4 <= litEndA && litPtrB + 4 <= litBufEnd) {
		BITStream_Read_Flush(bitStream);
		S_READ_FLUSH_BACK(bitStreamB);
		if (bitStream.streamPtr > bodyEnd + 8 || bitStreamB.streamPtr < body - 8) return -1;
		BITStream_Read_ExtHufX1(bitStream, remLit, litDemap, litPtr[0]);
		BITStream_Read_ExtHufX1(bitStreamB, remLit, litDemap, litPtrB[0]);
		BITStream_Read_ExtHufX1(bitStream, remLit, litDemap, litPtr[1]);
		BITStream_Read_ExtHufX1(bitStreamB, remLit, litDemap, litPtrB[1]);
		BITStream_Read_ExtHufX1(bitStream, remLit, litDemap, litPtr[2]);
		BITStream_Read_ExtHufX1(bitStreamB, remLit, litDemap, litPtrB[2]);
		BITStream_Read_ExtHufX1(bitStream, remLit, litDemap, litPtr[3]);
		BITStream_Read_ExtHufX1(bitStreamB, remLit, litDemap, litPtrB[3]);
		litPtr += 4;
		litPtrB += 4;
	}
	while (litPtr + 4 <= litEndA) {
		BITStream_Read_Flush(bitStream);
		if (bitStream.streamPtr > bodyEnd + 8) return -1;
		BITStream_Read_ExtHufX1(bitStream, remLit, litDemap, litPtr[0]);
		BITStream_Read_ExtHufX1(bitStream, remLit, litDemap, litPtr[1]);
		BITStream_Read_ExtHufX1(bitStream, remLit, litDemap, litPtr[2]);
		BITStream_Read_ExtHufX1(bitStream, remLit, litDemap, litPtr[3]);
		litPtr += 4;
	}
	BITStream_Read_Flush(bitStream);
	while (litPtr < litEndA) { BITStream_Read_ExtHufX1(bitStream, remLit, litDemap, *litPtr); litPtr++; }
	while (litPtrB + 4 <= litBufEnd) {
		S_READ_FLUSH_BACK(bitStreamB);
		if (bitStreamB.streamPtr < body - 8) return -1;
		BITStream_Read_ExtHufX1(bitStreamB, remLit, litDemap, litPtrB[0]);
		BITStream_Read_ExtHufX1(bitStreamB, remLit, litDemap, litPtrB[1]);
		BITStream_Read_ExtHufX1(bitStreamB, remLit, litDemap, litPtrB[2]);
		BITStream_Read_ExtHufX1(bitStreamB, remLit, litDemap, litPtrB[3]);
		litPtrB += 4;
	}
	S_READ_FLUSH_BACK(bitStreamB);
	while (litPtrB < litBufEnd) { BITStream_Read_ExtHufX1(bitStreamB, remLit, litDemap, *litPtrB); litPtrB++; }
	litPtr = litBuf;

	/* sequences: literal run and match length from the main stream, offsets from stream B */
	while (1) {
		Uint32 sym, lenSym, extra, litRun, mchLen, offVal, offset;
		BITStream_Read_Flush(bitStream);
		if (bitStream.streamPtr > bodyEnd + 8) return -1;

		BITStream_Read_ExtHufX1(bitStream, remLitRun, litRunDemap, sym);
		S_READ_BITS(bitStream, valueBits[sym], extra);
		litRun = valueBase[sym] + extra;
		if (litRun > (Uint32)(destEnd - destPtr) || litRun > (Uint32)(litBufEnd - litPtr)) return -1;

		if (destPtr + litRun + 16 <= destEnd) {             /* bulk copy; the overshoot is rewritten later */
			Uint8* const litEnd = destPtr + litRun;
			do { MemCopy16(destPtr, litPtr); destPtr += 16; litPtr += 16; } while (destPtr < litEnd);
			litPtr -= destPtr - litEnd;
			destPtr = litEnd;
		}
		else {
			memcpy(destPtr, litPtr, litRun);
			destPtr += litRun;
			litPtr += litRun;
		}
		if (destPtr == destEnd) {
			if (litPtr != litBufEnd) return -1;
			break;
		}

		/* the length symbol selects how the offset is coded */
		if (!mchLenMax) return -1;
		BITStream_Read_ExtHufX1(bitStream, remMchLen, mchLenDemap, lenSym);
		S_READ_BITS(bitStream, valueBits[lenSym + 1], extra);    /* symbols 0 and 1 have no extra bits */
		mchLen = lenSym < 2 ? 2 : valueBase[lenSym + 1] + extra;

		S_READ_FLUSH_BACK(bitStreamB);
		if (bitStreamB.streamPtr < body - 8) return -1;
		if (lenSym == 0) {                                    /* length 2: raw distance - 1 */
			if (!win.w2) return -1;
			BITStream_Read(bitStreamB, (Uint32)win.w2, extra);
			offset = extra + 1;
		}
		else if (lenSym == 1) {                               /* length 2 at repeat slot 0 */
			offset = rep[0];
		}
		else {
			if (!mchOffMax) return -1;
			Uint32 offSym;
			BITStream_Read_ExtHufX1(bitStreamB, remMchOff, mchOffDemap, offSym);
			if (offSym < 4) offVal = offSym;
			else {
				const Uint32 nExtra = (offSym >> 1) - 1;
				BITStream_Read(bitStreamB, nExtra, extra);
				offVal = ((2 | (offSym & 1)) << nExtra) | extra;
			}
			offset = S_Update_Rep(rep, offVal);
		}
		const Uint32 produced = (Uint32)(destPtr - dest);
		if (offset == 0 || offset > produced + (Uint32)histDict || mchLen > (Uint32)(destEnd - destPtr)) return -1;

		if (offset > produced) {
			/* the match starts in the dictionary and may continue into the block */
			const Uint32 inDict = Min(mchLen, offset - produced);
			memcpy(destPtr, dictEnd - (offset - produced), inDict);
			for (Uint32 k = inDict; k < mchLen; k++) destPtr[k] = dest[k - inDict];   /* position produced + k - offset */
			destPtr += mchLen;
			continue;
		}
		const Uint8* matchPtr = destPtr - offset;
		Uint8* const mchEnd = destPtr + mchLen;
		if (offset >= 16 && mchEnd + 16 <= destEnd) {
			do { MemCopy16(destPtr, matchPtr); destPtr += 16; matchPtr += 16; } while (destPtr < mchEnd);
			destPtr = mchEnd;
		}
		else if (offset >= 8 && mchEnd + 8 <= destEnd) {
			do { MemCopy8(destPtr, matchPtr); destPtr += 8; matchPtr += 8; } while (destPtr < mchEnd);
			destPtr = mchEnd;
		}
		else if (mchEnd + 8 <= destEnd) {
			/* offset 1..7: spread the first 8 bytes so that the source trails by a multiple of the
			   offset of at least 8 bytes, then copy 8 bytes at a time */
			static const Uint8 inc4[8] = { 0, 0, 0, 1, 0, 4, 4, 4 };     /* 4 % offset, as a source shift */
			static const Uint8 inc8[8] = { 0, 0, 0, 2, 0, 3, 2, 1 };     /* 8 % offset */
			destPtr[0] = matchPtr[0];
			destPtr[1] = matchPtr[1];
			destPtr[2] = matchPtr[2];
			destPtr[3] = matchPtr[3];
			memcpy(destPtr + 4, matchPtr + inc4[offset], 4);
			matchPtr += inc8[offset];
			destPtr += 8;
			while (destPtr < mchEnd) { MemCopy8(destPtr, matchPtr); destPtr += 8; matchPtr += 8; }
			destPtr = mchEnd;
		}
		else {
			while (destPtr < mchEnd) *destPtr++ = *matchPtr++;
		}
	}

	/* the main stream must end exactly where stream B begins */
	BITStream_Read_FlushEnd(bitStream);
	if (bitStream.streamPtr != bitStreamB.streamPtr + 8 - ((bitStreamB.nUsedBits + 7) >> 3)) return -1;
	return decSize;
}

static int S_Decompress_Default(const void* src, int srcSize, void* dst, int dstCap, const void* dict, int dictSize)
{
	return S_Decompress_Body(src, srcSize, dst, dstCap, dict, dictSize);
}

/* The same decoder compiled for BMI2, whose shifts by a register count take one micro-op and leave the flags alone;
   chosen at run time, as zstd does. */
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
static __attribute__((target("bmi,bmi2,lzcnt")))
int S_Decompress_Bmi2(const void* src, int srcSize, void* dst, int dstCap, const void* dict, int dictSize)
{
	return S_Decompress_Body(src, srcSize, dst, dstCap, dict, dictSize);
}
static int CPU_Has_Bmi2(void)
{
	static int has = -1;
	if (has < 0) {
		__builtin_cpu_init();
		has = __builtin_cpu_supports("bmi2") != 0;
	}
	return has;
}
#endif

int WZIPS_decompress_usingDict(const void* src, int srcSize, void* dst, int dstCap, const void* dict, int dictSize)
{
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
	if (CPU_Has_Bmi2()) return S_Decompress_Bmi2(src, srcSize, dst, dstCap, dict, dictSize);
#endif
	return S_Decompress_Default(src, srcSize, dst, dstCap, dict, dictSize);
}
