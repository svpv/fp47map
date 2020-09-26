// Copyright (c) 2019, 2020 Alexey Tourbin
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "fp47m.h"
#include <smmintrin.h>

static const struct {
    union {
	uint32_t init[64];
	__m128i leftpack[16];
    };
#ifndef __POPCNT__
    uint8_t popcnt[16];
#endif
} lut = {
    {{
/* 0000 */         -1,         -1,         -1,         -1,
/* 0001 */ 0x03020100,         -1,         -1,         -1,
/* 0010 */ 0x07060504,         -1,         -1,         -1,
/* 0011 */ 0x03020100, 0x07060504,         -1,         -1,
/* 0100 */ 0x0b0a0908,         -1,         -1,         -1,
/* 0101 */ 0x03020100, 0x0b0a0908,         -1,         -1,
/* 0110 */ 0x07060504, 0x0b0a0908,         -1,         -1,
/* 0111 */ 0x03020100, 0x07060504, 0x0b0a0908,         -1,
/* 1000 */ 0x0f0e0d0c,         -1,         -1,         -1,
/* 1001 */ 0x03020100, 0x0f0e0d0c,         -1,         -1,
/* 1010 */ 0x07060504, 0x0f0e0d0c,         -1,         -1,
/* 1011 */ 0x03020100, 0x07060504, 0x0f0e0d0c,         -1,
/* 1100 */ 0x0b0a0908, 0x0f0e0d0c,         -1,         -1,
/* 1101 */ 0x03020100, 0x0b0a0908, 0x0f0e0d0c,         -1,
/* 1110 */ 0x07060504, 0x0b0a0908, 0x0f0e0d0c,         -1,
/* 1111 */ 0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
    }},
#ifndef __POPCNT__
    {
/* 0000 */ 0,
/* 0001 */ 1,
/* 0010 */ 1,
/* 0011 */ 2,
/* 0100 */ 1,
/* 0101 */ 2,
/* 0110 */ 2,
/* 0111 */ 3,
/* 1000 */ 1,
/* 1001 */ 2,
/* 1010 */ 2,
/* 1011 */ 3,
/* 1100 */ 2,
/* 1101 */ 3,
/* 1110 */ 3,
/* 1111 */ 4,
    },
#endif
};

#ifndef __POPCNT__
#define popcnt4(x) lut.popcnt[x]
#else
#define popcnt4(x) (unsigned)__builtin_popcount(x)
#endif

#define ctz32(x) (unsigned)__builtin_ctz(x)

union buck2 {
    __m128 ps;
    __m128i x;
    union bent be[2];
};

struct buck4 {
    union {
	__m128i xtag;
	uint32_t tag[4];
    };
    union {
	__m128i xpos;
	uint32_t pos[4];
    };
};

void FASTCALL fp47m_prefetch2_sse4(uint64_t fp, const struct fp47map *map)
{
    dFP2I;
    union buck2 *bb = map->bb;
    __builtin_prefetch(&bb[i1]);
    __builtin_prefetch(&bb[i2]);
}

static void FASTCALL fp47m_prefetch4_sse4(uint64_t fp, const struct fp47map *map)
{
    dFP2I;
    struct buck4 *bb = map->bb;
    __builtin_prefetch(&bb[i1]);
    __builtin_prefetch(&bb[i2]);
}

static void FASTCALL fp47m_prefetch4re_sse4(uint64_t fp, const struct fp47map *map)
{
    dFP2I;
    ResizeI;
    struct buck4 *bb = map->bb;
    __builtin_prefetch(&bb[i1]);
    __builtin_prefetch(&bb[i2]);
}

static inline unsigned find2(__m128 xb1, __m128 xb2, uint32_t tag, void *mpos)
{
    __m128i xtag = _mm_castps_si128(_mm_shuffle_ps(xb1, xb2, _MM_SHUFFLE(2, 0, 2, 0)));
    __m128i xpos = _mm_castps_si128(_mm_shuffle_ps(xb1, xb2, _MM_SHUFFLE(3, 1, 3, 1)));
    __m128i xcmp = _mm_cmpeq_epi32(xtag, _mm_set1_epi32(tag));
    unsigned mask = _mm_movemask_ps(_mm_castsi128_ps(xcmp));
    _mm_storeu_si128(mpos, _mm_shuffle_epi8(xpos, lut.leftpack[mask]));
    return popcnt4(mask);
}

unsigned FASTCALL fp47m_find2_sse4(uint64_t fp, const struct fp47map *map, uint32_t *mpos)
{
    dFP2I;
    __m128 *bb = map->bb;
    return find2(bb[i1], bb[i2], tag, mpos);
}

// Turn an array of 2 entries per bucket (buck2) into an array
// of 4 non-interleaved entries per bucket (buck4).
static inline void reinterp24(__m128i *bb, size_t nb, __m128i *bb4)
{
    //                       p1b p3b  .  .        .   .   .   .
    //                       t1b t3b  .  .        .   .   .   .
    //                       p1a p3a  .  .       p0b p1b p2b p3b
    //                       t1a t3a  .  .       p0a p1a p2a p3a
    //  p0b p1b p2b p3b  =>  p0b p2b  .  .   =>   .   .   .   .
    //  t0b t1b t2b t3b      t0b t2b  .  .        .   .   .   .
    //  p0a p1a p2a p3a      p0a p2a  .  .       t0b t1b t2b t3b
    //  t0a t1a t2a t3a      t0a t2a  .  .       t0a t1a t2a t3a

    for (size_t i = nb; i; i -= 2) {
	__m128i b2 = bb[i-2];
	__m128i b3 = bb[i-1];
	__m128i t2 = _mm_shuffle_epi8(b2, _mm_setr_epi32(0x03020100, 0x0b0a0908, -1, -1));
	__m128i t3 = _mm_shuffle_epi8(b3, _mm_setr_epi32(0x03020100, 0x0b0a0908, -1, -1));
	__m128i p2 = _mm_shuffle_epi8(b2, _mm_setr_epi32(0x07060504, 0x0f0e0d0c, -1, -1));
	__m128i p3 = _mm_shuffle_epi8(b3, _mm_setr_epi32(0x07060504, 0x0f0e0d0c, -1, -1));
	bb4[2*i-4] = t2;
	bb4[2*i-3] = p2;
	bb4[2*i-2] = t3;
	bb4[2*i-1] = p3;
    }
}

static unsigned FASTCALL fp47m_find4_sse4(uint64_t fp, const struct fp47map *map, uint32_t *mpos);
static int FASTCALL fp47m_insert4_sse4(uint64_t fp, struct fp47map *map, uint32_t pos);

static inline int insert2tail(struct fp47map *map, uint32_t i1, uint32_t tag, uint32_t pos)
{
    size_t nb = map->mask0 + (size_t) 1;
    void *mem = realloc(map->bb, nb * 32 + 16);
    if (!mem)
	return -1;
    // Realign bb to a 32-byte boundary.
    map->bboff = (uintptr_t) mem & 16;
    map->bb = mem + map->bboff;
    reinterp24(mem, nb, map->bb);
    map->bsize = 4;
    // Insert kbe at i1, no kicks required.
    struct buck4 *bb = map->bb;
    bb[i1].tag[2] = tag;
    bb[i1].pos[2] = pos;
    map->find = fp47m_find4_sse4;
    map->insert = fp47m_insert4_sse4;
    map->prefetch = fp47m_prefetch4_sse4;
    return 2;
}

int FASTCALL fp47m_insert2_sse4(uint64_t fp, struct fp47map *map, uint32_t pos)
{
    dFP2I;
    union buck2 *bb = map->bb;
    union buck2 *b1 = &bb[i1];
    union buck2 *b2 = &bb[i2];
    __m128i xtag = _mm_castps_si128(_mm_shuffle_ps(b1->ps, b2->ps, _MM_SHUFFLE(2, 0, 2, 0)));
    __m128i xcmp = _mm_cmpeq_epi32(_mm_setzero_si128(), xtag);
    unsigned slots = _mm_movemask_epi8(_mm_shuffle_epi32(xcmp, _MM_SHUFFLE(3, 1, 2, 0)));
    map->cnt++;
    if (likely(slots)) {
	unsigned slot1 = ctz32(slots);
	b1 = (slot1 & 4) ? b2 : b1;
	union bent *be = &b1->be[slot1>>3];
	be->tag = tag, be->pos = pos;
	return 1;
    }
    if (1) { // check the fill factor
	unsigned mask0 = map->mask0;
	int maxkick = 2 * map->logsize0;
	// This entry kicks!
	__m128i kbe = _mm_cvtsi32_si128(tag);
	kbe = _mm_insert_epi32(kbe, pos, 1);
	do {
	    // This entry is kicked out.
	    __m128i obe = b1->x;
	    i1 ^= b1->be[0].tag;
	    b1->x = _mm_alignr_epi8(kbe, obe, 8);
	    i1 &= mask0;
	    b1 = &bb[i1];
	    if (b1->be[0].tag == 0) return _mm_storel_epi64((void *) &b1->be[0], obe), 1;
	    if (b1->be[1].tag == 0) return _mm_storel_epi64((void *) &b1->be[1], obe), 1;
	    kbe = obe;
	} while (--maxkick >= 0);
	tag = _mm_cvtsi128_si32(kbe);
	pos = _mm_extract_epi32(kbe, 1);
    }
    return insert2tail(map, i1, tag, pos);
}

static inline unsigned find4(__m128i xtag, __m128i xpos, uint32_t tag, void *mpos)
{
    __m128i xcmp = _mm_cmpeq_epi32(xtag, _mm_set1_epi32(tag));
    unsigned mask = _mm_movemask_ps(_mm_castsi128_ps(xcmp));
    _mm_storeu_si128(mpos, _mm_shuffle_epi8(xpos, lut.leftpack[mask]));
    return popcnt4(mask);
}

static unsigned FASTCALL fp47m_find4_sse4(uint64_t fp, const struct fp47map *map, uint32_t *mpos)
{
    dFP2I;
    struct buck4 *bb = map->bb;
    unsigned n = find4(bb[i1].xtag, bb[i1].xpos, tag, mpos);
    return   n + find4(bb[i2].xtag, bb[i2].xpos, tag, mpos + n);
}

static unsigned FASTCALL fp47m_find4re_sse4(uint64_t fp, const struct fp47map *map, uint32_t *mpos)
{
    dFP2I;
    ResizeI;
    struct buck4 *bb = map->bb;
    unsigned n = find4(bb[i1].xtag, bb[i1].xpos, tag, mpos);
    return   n + find4(bb[i2].xtag, bb[i2].xpos, tag, mpos + n);
}

static inline void reinterp44(struct buck4 *bb, size_t nb, struct buck4 *bb4,
	uint32_t mask0, uint32_t mask1)
{
    struct buck4 *bb8 = bb4 + nb;
    __m128i xmul = _mm_set1_epi32(mask0 + 1);
    __m128i xmask0 = _mm_set1_epi32(mask0);
    __m128i xmask1 = _mm_set1_epi32(mask1);
    for (size_t i = 0; i < nb; i++) {
	__m128i xtag = bb[i].xtag;
	__m128i xhi = _mm_mullo_epi32(xtag, xmul);
	__m128i xi1 = _mm_set1_epi32(i & mask0);
	__m128i xi2 = _mm_and_si128(_mm_xor_si128(xi1, xtag), xmask0);
	xi1 = _mm_blendv_epi8(xi1, xi2, _mm_cmpgt_epi32(xi1, xi2));
	xi1 = _mm_or_si128(xi1, xhi);
	xi2 = _mm_xor_si128(xi1, xtag);
	xi1 = _mm_and_si128(xi1, xmask1);
	xi2 = _mm_and_si128(xi2, xmask1);
	__m128i xi = _mm_set1_epi32(i);
	__m128i xeq1 = _mm_cmpeq_epi32(xi1, xi);
	__m128i xeq2 = _mm_cmpeq_epi32(xi2, xi);
	__m128i xeq = _mm_or_si128(xeq1, xeq2);
	unsigned slots4 = _mm_movemask_ps(_mm_castsi128_ps(xeq));
	unsigned slots8 = ~slots4 & 15;
	__m128i xpos = bb[i].xpos;
	bb4[i].xtag = _mm_shuffle_epi8(xtag, lut.leftpack[slots4]);
	bb8[i].xtag = _mm_shuffle_epi8(xtag, lut.leftpack[slots8]);
	bb4[i].xpos = _mm_shuffle_epi8(xpos, lut.leftpack[slots4]);
	bb8[i].xpos = _mm_shuffle_epi8(xpos, lut.leftpack[slots8]);
    }
}

static inline bool insert4(struct buck4 *b1, struct buck4 *b2, uint32_t tag, uint32_t pos)
{
    __m128i xcmp1 = _mm_cmpeq_epi32(_mm_setzero_si128(), b1->xtag);
    __m128i xcmp2 = _mm_cmpeq_epi32(_mm_setzero_si128(), b2->xtag);
    unsigned slots = _mm_movemask_epi8(_mm_blend_epi16(xcmp1, xcmp2, 0xaa));
    if (likely(slots)) {
	unsigned slot1 = ctz32(slots);
	b1 = (slot1 & 2) ? b2 : b1;
	b1->tag[slot1>>2] = tag;
	b1->pos[slot1>>2] = pos;
	return true;
    }
    return false;
}

static inline bool kickloop4(struct buck4 *bb, struct buck4 *b1,
	uint32_t *i1, uint32_t *tag, uint32_t *pos, uint32_t mask, int maxkick)
{
    __m128i ktag = _mm_cvtsi32_si128(*tag);
    __m128i kpos = _mm_cvtsi32_si128(*pos);
#define i1 (*i1)
    do {
	__m128i otag = b1->xtag;
	__m128i opos = b1->xpos;
	i1 ^= b1->tag[0];
	b1->xtag = _mm_alignr_epi8(ktag, otag, 4);
	b1->xpos = _mm_alignr_epi8(kpos, opos, 4);
	i1 &= mask;
	b1 = &bb[i1];
	__m128i xcmp = _mm_cmpeq_epi32(b1->xtag, _mm_setzero_si128());
	unsigned slots = _mm_movemask_epi8(xcmp);
	if (likely(slots)) {
	    unsigned slot1 = ctz32(slots);
	    b1->tag[slot1>>2] = _mm_cvtsi128_si32(otag);
	    b1->pos[slot1>>2] = _mm_cvtsi128_si32(opos);
	    return true;
	}
	ktag = otag, kpos = opos;
    } while (--maxkick >= 0);
#undef i1
    *tag = _mm_cvtsi128_si32(ktag);
    *pos = _mm_cvtsi128_si32(kpos);
    return false;
}

static int FASTCALL fp47m_insert4re_sse4(uint64_t fp, struct fp47map *map, uint32_t pos);

static int fp47m_insert4tail_sse4(struct fp47map *map, uint32_t i1, uint32_t tag, uint32_t pos)
{
    if (map->logsize1 == 32)
	return errno = E2BIG, -1;
    if (map->logsize1 == 26 && sizeof(size_t) < 5)
	return errno = ENOMEM, -1;
    size_t nb = map->mask1 + (size_t) 1;
    void *mem = realloc(map->bb - map->bboff, 2 * nb * 32 + 16);
    if (!mem)
	return -1;
    // XXX Realign bb to a 32-byte boundary.
    assert(map->bboff == ((uintptr_t) mem & 16));
    map->bb = mem + map->bboff;
    map->mask1 = map->mask1 << 1 | 1;
    map->logsize1++;
    map->find = fp47m_find4re_sse4;
    map->insert = fp47m_insert4re_sse4;
    map->prefetch = fp47m_prefetch4re_sse4;
    reinterp44(map->bb, nb, map->bb, map->mask0, map->mask1);
    // Reinsert the stashed entries and the pending entry.
    for (int j = 0; j < 1; j++) {
	uint32_t i2 = i1 ^ tag;
	i1 &= map->mask0;
	i2 &= map->mask0;
	ResizeI;
	struct buck4 *bb = map->bb;
	struct buck4 *b1 = &bb[i1];
	if (insert4(b1, &bb[i2], tag, pos))
	    continue;
	if (kickloop4(bb, b1, &i1, &tag, &pos, map->mask1, 2 * map->logsize1))
	    continue;
	return -1;
    }
    return 2;
}

static inline int insert4tmpl(struct fp47map *map, uint64_t fp, uint32_t pos, bool re)
{
    dFP2I;
    if (re)
	ResizeI;
    struct buck4 *bb = map->bb;
    struct buck4 *b1 = &bb[i1];
    map->cnt++;
    if (insert4(b1, &bb[i2], tag, pos))
	return 1;
    if (1) { // check the fill factor
	unsigned mask = re ? map->mask1 : map->mask0;
	int logsize = re ? map->logsize1 : map->logsize0;
	if (kickloop4(bb, b1, &i1, &tag, &pos, mask, 2 * logsize))
	    return 1;
    }
    return fp47m_insert4tail_sse4(map, i1, tag, pos);
}

static int FASTCALL fp47m_insert4_sse4(uint64_t fp, struct fp47map *map, uint32_t pos)
{
    return insert4tmpl(map, fp, pos, 0);
}

static int FASTCALL fp47m_insert4re_sse4(uint64_t fp, struct fp47map *map, uint32_t pos)
{
    return insert4tmpl(map, fp, pos, 1);
}
