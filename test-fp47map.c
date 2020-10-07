// Copyright (c) 2020 Alexey Tourbin
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

#undef NDEBUG
#include <stdio.h>
#include <inttypes.h>
#include "fp47m.h"

// A hashing primitive, by Pelle Evensen.
static inline uint64_t nasam(uint64_t x)
{
#define ror64(x, k) (x >> k | x << (64 - k))
    x ^= ror64(x, 25) ^ ror64(x, 47);
    x *= 0x9e6c63d0676a9a99;
    x ^= x >> 23 ^ x >> 51;
    x *= 0x9e6d62d06f6a9a9b;
    x ^= x >> 23 ^ x >> 51;
    return x;
}

// Recheck that all elements are accessible.
static void recheck(struct fp47map *map, unsigned imax)
{
    unsigned e0 = 0; // found more than one
    unsigned e1 = 0; // false positives
    for (unsigned i = 1; i <= imax; i += 2) {
	uint32_t mpos[FP47MAP_MAXFIND];
	unsigned n = fp47map_find(map, nasam(i), mpos);
	assert(n > 0);
	assert(mpos[0] == i || (n > 1 && mpos[1] == i));
	e0 += n - 1;
	e1 += fp47map_find(map, nasam(i + 1), mpos);
    }
    assert(map->cnt + map->nstash == imax / 2 + 1);
    assert(e0 <= 3);
    assert(e1 <= 1);
}

// Insert pseudorandom data and hash the buckets after a few resizes.
static uint64_t test(bool sse4)
{
    struct fp47map *map = fp47map_new(10);
    assert(map);
    if (sse4) {
#if defined(__i386__) || defined(__x86_64__)
	map->find = fp47m_find2_sse4;
	map->insert = fp47m_insert2_sse4;
	map->prefetch = fp47m_prefetch2_sse4;
    }
    else {
	map->find = fp47m_find2;
	map->insert = fp47m_insert2;
	map->prefetch = fp47m_prefetch2;
#endif
    }
    for (unsigned i = 1; i <= UINT16_MAX; i += 2) {
	unsigned nstash = map->nstash;
	int rc = fp47map_insert(map, nasam(i), i);
	assert(rc > 0);
	if (rc == 2 || map->nstash != nstash)
	    recheck(map, i);
    }
    uint32_t *bb = map->bb;
    uint32_t *bbend = bb + 8 * (map->mask1 + 1);
    uint64_t h = 0x5851f42d4c957f2d;
    do {
	uint64_t x0, x1, y0, y1;
#if defined(__i386__) || defined(__x86_64__)
	if (sse4) {
	    x0 = bb[0] | (uint64_t) bb[1] << 32; // tag0 tag1
	    x1 = bb[2] | (uint64_t) bb[3] << 32; // tag2 tag3
	    y0 = bb[4] | (uint64_t) bb[5] << 32; // pos0 pos1
	    y1 = bb[6] | (uint64_t) bb[7] << 32; // pos2 pos3
	}
	else
#endif
	{
	    x0 = bb[0] | (uint64_t) bb[2] << 32;
	    x1 = bb[4] | (uint64_t) bb[6] << 32;
	    y0 = bb[1] | (uint64_t) bb[3] << 32;
	    y1 = bb[5] | (uint64_t) bb[7] << 32;
	}
	assert((y0 & 0xffff0000ffff0000) == 0);
	assert((y1 & 0xffff0000ffff0000) == 0);
	y0 |= y0 << 16;
	y1 |= y1 << 16;
	h = nasam((h ^ x0) + y0);
	h = nasam((h ^ x1) + y1);
	bb += 8;
    } while (bb != bbend);
    fp47map_free(map);
    return h;
}

int main()
{
   uint64_t h0 = test(true);
   uint64_t h1 = test(false);
   assert(h0 == h1);
   printf("%016" PRIx64 "\n", h0);
   return 0;
}
