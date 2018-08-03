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

#include "fpmap.h"

// The bucket entry, maps fp32->pos.  Note however that the match is determined
// by the right fingerprint at the right bucket.  The same 32-bit fingerprint
// can have different meaning depending on its location in the table.
struct bent {
    uint32_t fp32;
    uint32_t pos;
} __attribute__((aligned(8)));

// The real fpmap structure (upconverted from "void *map").
struct map {
    // Virtual functions.
    size_t (*find)(void *map, uint64_t fp, uint32_t pos[10]);
    bool (*insert)(void *map, uint64_t fp, uint32_t pos);
    // The number of slots in each bucket: 2, 3, or 4.
    uint8_t bsize;
    // The number of fingerprints stashed: 0, 1, or 2.
    uint8_t nstash;
    // The number of buckets, initial and current, the logarithm: 3..30.
    uint8_t logsize0, logsize1;
    // The corresponding masks, help indexing into the buckets.
    uint32_t mask0, mask1;
    // The total number of entries in the buckets, not including the stash.
    uint32_t cnt;
    // The buckets (malloc'd); each bucket has bsize slots.
    // Two-dimensional structure is emulated with pointer arithmetic.
    struct bent *bb;
    // To reduce the failure rate, one or two entries can be stashed.
    struct stash {
	uint64_t key[2];
	uint32_t pos[2];
    } stash;
};

#define unlikely(cond) __builtin_expect(cond, 0)

// The inline functions below rely heavily on constant propagation.
#define inline inline __attribute__((always_inline))

// How do we sort two numbers?  That's a major problem in computer science.
// I thought that cmov might help, but this does not seem to be the case.
#define Sort2asm(i1, i2)		\
    do {				\
	size_t i3;			\
	asm("cmp %[i1],%[i2]\n\t"	\
	    "cmovb %[i1],%[i3]\n\t"	\
	    "cmovb %[i2],%[i1]\n\t"	\
	    "cmovb %[i3],%[i2]\n\t"	\
	    : [i1] "+r" (i1),		\
	      [i2] "+r" (i2),		\
	      [i3] "=rm" (i3)		\
	    :: "cc");			\
    } while (0)
#define Sort2cmov(i1, i2)		\
    do {				\
	size_t i3 = i1;			\
	size_t i4 = i2;			\
	i1 = (i1 > i2) ? i2 : i1;	\
	i2 = (i3 > i4) ? i3 : i2;	\
    } while (0)
#define Sort2swap(i1, i2)		\
    do {				\
	if (i1 > i2) {			\
	    size_t i3 = i1;		\
	    i1 = i2, i2 = i3;		\
	}				\
    } while (0)
#define Sort2 Sort2swap

// Template for map->find virtual functions.
static inline size_t t_find(struct map *map, uint64_t fp, uint32_t pos[10],
	uint8_t bsize, bool resized, uint8_t nstash)
{
    // So a fingerprint is digested into a smaller 32-bit fingerprint (by
    // simply taking lower 32 bits) and two buckets at which it can reside.
    // Note that the buckets are completely symmetrical with regard to "xor fp32",
    // i.e. the information about "the first and true" index is not preserved.
    // This looses about 1 bit out of 32+logsize bits of hashing material.
    uint32_t fp32 = fp;
    size_t i1 = fp >> 32;
    size_t i2 = i1 ^ fp;
    i1 &= map->mask0;
    i2 &= map->mask0;

    // Need to sort i1 < i2?
    if (resized || nstash)
	Sort2(i1, i2);

    // Key with canonical index, to look up in the stash.
    uint64_t stkey;
    if (nstash)
	stkey = fp32 | (uint64_t) i1 << 32;

    // Indexes need extra high bits?
    if (resized) {
	i1 |= fp32 << map->logsize0;
	i2 = i1 ^ fp;
	i1 &= map->mask1;
	i2 &= map->mask1;
    }

    // Indexes are ready, at last the buckets.
    struct bent *b1 = map->bb + i1 * bsize;
    struct bent *b2 = map->bb + i2 * bsize;
    if (nstash)
	__builtin_prefetch(b1);
    __builtin_prefetch(b2);

    // The result.
    size_t n = 0;

    // Branches are predictable, no need for cmov.
#define AddMatch(Key, Fp, Pos)		\
    do {				\
	if (unlikely(Key == Fp))	\
	    pos[n++] = Pos;		\
    } while (0)

    // Check the stash.
    struct stash *st = &map->stash;
    if (nstash > 0) AddMatch(stkey, st->key[0], st->pos[0]);
    if (nstash > 1) AddMatch(stkey, st->key[1], st->pos[1]);

    // Check the first bucket.
    AddMatch(fp32, b1[0].fp32, b1[0].pos);
    AddMatch(fp32, b1[1].fp32, b1[1].pos);
    if (bsize > 2) AddMatch(fp32, b1[2].fp32, b1[2].pos);
    if (bsize > 3) AddMatch(fp32, b1[3].fp32, b1[3].pos);

    // The second bucket has been prefetched by now.
    AddMatch(fp32, b2[0].fp32, b2[0].pos);
    AddMatch(fp32, b2[1].fp32, b2[1].pos);
    if (bsize > 2) AddMatch(fp32, b2[2].fp32, b2[2].pos);
    if (bsize > 3) AddMatch(fp32, b2[3].fp32, b2[3].pos);

    return n;
}

// Virtual functions for map->find.
static size_t fpmap_find2re0st0(void *map, uint64_t fp, uint32_t pos[10]) { return t_find(map, fp, pos, 2, 0, 0); }
static size_t fpmap_find2re0st1(void *map, uint64_t fp, uint32_t pos[10]) { return t_find(map, fp, pos, 2, 0, 1); }
static size_t fpmap_find2re0st2(void *map, uint64_t fp, uint32_t pos[10]) { return t_find(map, fp, pos, 2, 0, 2); }
static size_t fpmap_find3re0st0(void *map, uint64_t fp, uint32_t pos[10]) { return t_find(map, fp, pos, 3, 0, 0); }
static size_t fpmap_find3re0st1(void *map, uint64_t fp, uint32_t pos[10]) { return t_find(map, fp, pos, 3, 0, 1); }
static size_t fpmap_find3re0st2(void *map, uint64_t fp, uint32_t pos[10]) { return t_find(map, fp, pos, 3, 0, 2); }
static size_t fpmap_find3re1st0(void *map, uint64_t fp, uint32_t pos[10]) { return t_find(map, fp, pos, 3, 1, 0); }
static size_t fpmap_find3re1st1(void *map, uint64_t fp, uint32_t pos[10]) { return t_find(map, fp, pos, 3, 1, 1); }
static size_t fpmap_find3re1st2(void *map, uint64_t fp, uint32_t pos[10]) { return t_find(map, fp, pos, 3, 1, 2); }
static size_t fpmap_find4re0st0(void *map, uint64_t fp, uint32_t pos[10]) { return t_find(map, fp, pos, 4, 0, 0); }
static size_t fpmap_find4re0st1(void *map, uint64_t fp, uint32_t pos[10]) { return t_find(map, fp, pos, 4, 0, 1); }
static size_t fpmap_find4re0st2(void *map, uint64_t fp, uint32_t pos[10]) { return t_find(map, fp, pos, 4, 0, 2); }
static size_t fpmap_find4re1st0(void *map, uint64_t fp, uint32_t pos[10]) { return t_find(map, fp, pos, 4, 1, 0); }
static size_t fpmap_find4re1st1(void *map, uint64_t fp, uint32_t pos[10]) { return t_find(map, fp, pos, 4, 1, 1); }
static size_t fpmap_find4re1st2(void *map, uint64_t fp, uint32_t pos[10]) { return t_find(map, fp, pos, 4, 1, 2); }

static inline bool justAdd(struct bent *b1, struct bent *b2,
	struct bent be, uint8_t bsize)
{
    if (b1[0].pos == 0) return b1[0] = be, true;
    if (b2[0].pos == 0) return b2[0] = be, true;
    if (b1[1].pos == 0) return b1[1] = be, true;
    if (b2[1].pos == 0) return b2[1] = be, true;
    if (bsize > 2 && b1[2].pos == 0) return b1[2] = be, true;
    if (bsize > 2 && b2[2].pos == 0) return b2[2] = be, true;
    if (bsize > 3 && b1[3].pos == 0) return b1[3] = be, true;
    if (bsize > 3 && b2[3].pos == 0) return b2[3] = be, true;
    return false;
}

#include <string.h>
#include <limits.h>

// On 64-bit systems, assume malloc'd chunks are aligned to 16 bytes.
// This should help to elicit aligned SSE2 instructions.
#if SIZE_MAX > UINT32_MAX
#define A16(p) __builtin_assume_aligned(p, 16)
#else
// Already assuming that "struct bent" is 8-byte aligned.
#define A16(p) __builtin_assume_aligned(p, 8)
#endif

// Unlike memcpy, memmove(dst, src, 16) may end up not inlined.
// Hence the specialized code to shift two bucket entries down.
static inline void MoveDown2(struct bent *b)
{
#if SIZE_MAX > UINT32_MAX || defined(__x86_64__)
    uint64_t buf[2];
    memcpy(buf, b + 1, 16);
    memcpy(b, buf, 16);
#else
    b[0] = b[1];
    b[1] = b[2];
#endif
}

static inline bool kickAdd(struct map *map, struct bent *b, struct bent be, size_t ix,
	struct bent *obe, size_t *oix, uint8_t bsize, bool resized)
{
    int k = 2 * map->logsize1;
    do {
	// Put at the top, kick out from the bottom.
	// Using *obe as a temporary register.
	*obe = b[0];
	if (bsize == 2)
	    b[0] = b[1];
	else if (bsize == 3)
	    MoveDown2(b);
	else {
	    MoveDown2(A16(b));
	    b[2] = b[3];
	}
	b[bsize-1] = be, be = *obe;
	// Ponder over the entry that's been kicked out.
	// Find out the alternative bucket.
	ix ^= be.fp32;
	ix &= resized ? map->mask1 : map->mask0;
	b = map->bb + bsize * ix;
	// Insert to the alternative bucket.
	if (b[0].pos == 0) return b[0] = be, true;
	if (b[1].pos == 0) return b[1] = be, true;
	if (bsize > 2 && b[2].pos == 0) return b[2] = be, true;
	if (bsize > 3 && b[3].pos == 0) return b[3] = be, true;
    } while (k-- > 0);
    // Ran out of tries? obe already set.
    *oix = ix;
    return false;
}

// Template for map->insert virtual functions.
static inline bool t_insert(struct map *map, uint64_t fp, uint32_t pos,
	uint8_t bsize, bool resized, uint8_t nstash)
{
    uint32_t fp32 = fp;
    size_t i1 = fp >> 32;
    size_t i2 = i1 ^ fp;
    i1 &= map->mask0;
    i2 &= map->mask0;

    if (resized) {
	Sort2(i1, i2);
	i1 |= fp32 << map->logsize0;
	i2 = i1 ^ fp;
	i1 &= map->mask1;
	i2 &= map->mask1;
    }

    struct bent be = { fp32, pos };
    struct bent *b1 = map->bb + i1 * bsize;
    struct bent *b2 = map->bb + i2 * bsize;

    if (justAdd(b1, b2, be, bsize))
	return map->cnt++, true;
    if (kickAdd(map, b1, be, i1, &be, &i1, bsize, resized))
	return map->cnt++, true;
    return false;
}

// Virtual functions for map->insert.
static bool fpmap_insert2re0st0(void *map, uint64_t fp, uint32_t pos) { return t_insert(map, fp, pos, 2, 0, 0); }
static bool fpmap_insert2re0st1(void *map, uint64_t fp, uint32_t pos) { return t_insert(map, fp, pos, 2, 0, 1); }
static bool fpmap_insert2re0st2(void *map, uint64_t fp, uint32_t pos) { return t_insert(map, fp, pos, 2, 0, 2); }
static bool fpmap_insert3re0st0(void *map, uint64_t fp, uint32_t pos) { return t_insert(map, fp, pos, 3, 0, 0); }
static bool fpmap_insert3re0st1(void *map, uint64_t fp, uint32_t pos) { return t_insert(map, fp, pos, 3, 0, 1); }
static bool fpmap_insert3re0st2(void *map, uint64_t fp, uint32_t pos) { return t_insert(map, fp, pos, 3, 0, 2); }
static bool fpmap_insert3re1st0(void *map, uint64_t fp, uint32_t pos) { return t_insert(map, fp, pos, 3, 1, 0); }
static bool fpmap_insert3re1st1(void *map, uint64_t fp, uint32_t pos) { return t_insert(map, fp, pos, 3, 1, 1); }
static bool fpmap_insert3re1st2(void *map, uint64_t fp, uint32_t pos) { return t_insert(map, fp, pos, 3, 1, 2); }
static bool fpmap_insert4re0st0(void *map, uint64_t fp, uint32_t pos) { return t_insert(map, fp, pos, 4, 0, 0); }
static bool fpmap_insert4re0st1(void *map, uint64_t fp, uint32_t pos) { return t_insert(map, fp, pos, 4, 0, 1); }
static bool fpmap_insert4re0st2(void *map, uint64_t fp, uint32_t pos) { return t_insert(map, fp, pos, 4, 0, 2); }
static bool fpmap_insert4re1st0(void *map, uint64_t fp, uint32_t pos) { return t_insert(map, fp, pos, 4, 1, 0); }
static bool fpmap_insert4re1st1(void *map, uint64_t fp, uint32_t pos) { return t_insert(map, fp, pos, 4, 1, 1); }
static bool fpmap_insert4re1st2(void *map, uint64_t fp, uint32_t pos) { return t_insert(map, fp, pos, 4, 1, 2); }

#include <assert.h>
#include <stdlib.h>
#include <errno.h>

struct fpmap *fpmap_new(int logsize)
{
    assert(logsize >= 0);
    if (logsize < 3)
	logsize = 3;
    if (logsize > 30)
	return errno = E2BIG, NULL;

    // Starting with two slots per bucket.
    size_t nb = (size_t) 1 << logsize;
    struct bent *bb = calloc(nb, 2 * sizeof(struct bent));
    if (!bb)
	return NULL;

    struct map *map = malloc(sizeof *map);
    if (!map)
	return free(bb), NULL;

    map->find = fpmap_find2re0st0;
    map->insert = fpmap_insert2re0st0;
    map->bsize = 2;
    map->nstash = 0;
    map->logsize0 = map->logsize1 = logsize;
    map->mask0 = map->mask1 = nb - 1;
    map->bb = bb;

    return (struct fpmap *) map;
}

void fpmap_free(struct fpmap *arg)
{
    struct map *map = (void *) arg;
    if (!map)
	return;
#ifdef FPMAP_DEBUG
    // The number of entries must match the occupied slots.
    size_t cnt = 0;
    struct bent *bb = map->bb;
    size_t n = map->bsize * (map->mask1 + 1);
    for (size_t i = 0; i < n; i += 4)
	cnt += (bb[i+0].pos > 0)
	    +  (bb[i+1].pos > 0)
	    +  (bb[i+2].pos > 0)
	    +  (bb[i+3].pos > 0);
    assert(cnt == map->cnt);
#endif
    free(map->bb);
    free(map);
}
