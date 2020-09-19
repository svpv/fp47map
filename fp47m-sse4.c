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

static inline unsigned find2(__m128 xb1, __m128 xb2, uint32_t fptag, void *mpos)
{
    __m128i xtag = _mm_castps_si128(_mm_shuffle_ps(xb1, xb2, _MM_SHUFFLE(2, 0, 2, 0)));
    __m128i xpos = _mm_castps_si128(_mm_shuffle_ps(xb1, xb2, _MM_SHUFFLE(3, 1, 3, 1)));
    __m128i xcmp = _mm_cmpeq_epi32(xtag, _mm_set1_epi32(fptag));
    unsigned mask = _mm_movemask_ps(_mm_castsi128_ps(xcmp));
    _mm_storeu_si128(mpos, _mm_shuffle_epi8(xpos, lut.leftpack[mask]));
    return popcnt4(mask);
}

unsigned FP47M_FASTCALL fp47m_find2_sse4(uint64_t fp, const struct fp47map *map, uint32_t *mpos)
{
    dFP2I;
    __m128 *bb = map->bb;
    return find2(bb[i1], bb[i2], fptag, mpos);
}
