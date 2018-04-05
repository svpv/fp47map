// Copyright (c) 2017, 2018 Alexey Tourbin
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

#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include "fpmap.h"

// Reduce a 64-bit fingerprint to [1,UINT32_MAX].
static inline uint32_t reduce32(uint64_t fp)
{
#ifdef __SIZEOF_INT128__
    // A fast alternative to the modulo reduction
    // https://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/
    return 1 + ((UINT32_MAX * (__uint128_t) fp) >> 64);
#endif
    // Can the compiler generate efficient code?
    if (sizeof(long) > 4)
	return 1 + fp % UINT32_MAX;
    // On i686, gcc generates the __umoddi3 call instead.
    // The following is equivalent to 1 + fp % UINT32_MAX.
    fp = (fp >> 32) + (uint32_t) fp + 1;
    return fp + (fp >> 32) + ((fp + 1) >> 33);
}

struct bent {
    uint32_t fp32;
    uint32_t pos;
};

struct map {
    uint32_t *(*get)(void *map, uint64_t);
    uint32_t *(*has)(void *map, uint64_t);
    uint32_t mask1, mask2;
    struct bent *bb;
    uint8_t logsize;
    uint8_t bsize;
};

static inline uint32_t *has(struct map *map, uint64_t fp, int bsize)
{
    uint32_t fp32 = reduce32(fp);
    size_t ix1 = fp & map->mask1;
    size_t ix2 = (ix1 ^ fp32) & map->mask2;
    struct bent *b1 = map->bb + ix1 * bsize;
    struct bent *b2 = map->bb + ix2 * bsize;
    for (int j = 0; j < bsize; j++) {
	if (b1[j].fp32 == fp32) return &b1[j].pos;
	if (b2[j].fp32 == fp32) return &b2[j].pos;
    }
    return NULL;
}

static uint32_t *b2_has(void *map, uint64_t fp) { return has(map, fp, 2); }
static uint32_t *b3_has(void *map, uint64_t fp) { return has(map, fp, 3); }
static uint32_t *b4_has(void *map, uint64_t fp) { return has(map, fp, 4); }

static inline bool kick(struct map *map, struct bent *b, size_t ix,
	struct bent be, struct bent *obe, int bsize)
{
    int maxk = 2 * map->logsize;
    for (int k = 1; k <= maxk; k++) {
	// Using *obe as a temporary register.
	*obe = b[0];
	for (int i = 1; i < bsize; i++)
	    b[i-1] = b[i];
	b[bsize-1] = be, be = *obe;
	ix = (ix ^ be.fp32) & map->mask2;
	b = map->bb + ix * bsize;
	for (int j = 0; j < bsize; j++)
	    if (b[j].fp32 == 0)
		return b[j] = be, true;
    }
    return false;
}

static inline uint32_t *get(struct map *map, uint64_t fp, int bsize)
{
    uint32_t fp32 = reduce32(fp);
    size_t ix1 = fp & map->mask1;
    size_t ix2 = (ix1 ^ fp32) & map->mask2;
    struct bent *b1 = map->bb + ix1 * bsize;
    struct bent *b2 = map->bb + ix2 * bsize;
    for (int j = 0; j < bsize; j++) {
	if (b1[j].fp32 == fp32) return &b1[j].pos;
	if (b2[j].fp32 == fp32) return &b2[j].pos;
    }
    for (int j = 0; j < bsize; j++) {
	if (b1[j].fp32 == 0) { b1[j].fp32 = fp32; return &b1[j].pos; }
	if (b2[j].fp32 == 0) { b2[j].fp32 = fp32; return &b2[j].pos; }
    }
    struct bent be = { fp32, 0 };
    if (kick(map, b1, ix1, be, &be, bsize)) {
	for (int j = bsize - 1; j >= 0; j--) {
	    if (b1[j].fp32 == fp32) return &b1[j].pos;
	    if (b2[j].fp32 == fp32) return &b2[j].pos;
	}
	assert(0);
    }
    return NULL;
}

static uint32_t *b2_get(void *map, uint64_t fp) { return get(map, fp, 2); }
static uint32_t *b3_get(void *map, uint64_t fp) { return get(map, fp, 3); }
static uint32_t *b4_get(void *map, uint64_t fp) { return get(map, fp, 4); }

struct fpmap *fpmap_new(int logsize)
{
    assert(logsize >= 0);
    if (logsize < 8)
	logsize = 8;
    if (logsize > 24)
	return errno = E2BIG, NULL;

    struct map *map = aligned_alloc(64, sizeof *map);
    if (!map)
	return NULL;

    map->has = b2_has;
    map->get = b2_get;

    map->logsize = logsize;
    map->mask1 = map->mask2 = (1 << logsize) - 1;

    map->bsize = 2;
    map->bb = calloc((1 << logsize), sizeof(*map->bb) * 2);
    if (!map->bb)
	return free(map), NULL;

    return (struct fpmap *) map;
}

void fpmap_free(struct fpmap *arg)
{
    struct map *map = (void *) arg;
    if (!map)
	return;
    free(map->bb);
    free(map);
}
