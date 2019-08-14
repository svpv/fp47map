// Copyright (c) 2017, 2018, 2019 Alexey Tourbin
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

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// The implementation does not depend on the caller's struct fpmap_bent
// exact definition, only on its size.
struct fpmap_bent {
#if FPMAP_FPTAG_BITS == 16
    unsigned char data[FPMAP_BENT_SIZE-2];
    uint16_t fptag;
#else
    unsigned char data[FPMAP_BENT_SIZE-4];
    uint32_t fptag;
#endif
};

#include "fpmap.h"

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

#define mod16(fp) (1 + (uint32_t)(fp) % UINT16_MAX)
#define mod32(fp) (1 + (fp) % UINT32_MAX)
#define Golden32 UINT32_C(2654435761)

#if FPMAP_FPTAG_BITS == 16
#define dI1T					\
    uint32_t i1 = fp >> 32;			\
    uint32_t fptag = mod16(fp);			\
    uint32_t xorme = fptag * Golden32
#else
#define dI1T					\
    uint32_t i1 = fp;				\
    uint32_t fptag = mod32(fp);			\
    uint32_t xorme = fptag
#endif

// Declare the variables which map the fingerprint to indexes and buckets.
// Note that the two buckets are completely symmetrical with regard to xor,
// i.e. the information about "the first and true" index is not preserved.
// This looses about 1 bit out of 32+logsize bits of hashing material.
#define dFP2IB					\
    dI1T;					\
    uint32_t i2 = i1 ^ xorme;			\
    i1 &= map->mask0;				\
    i2 &= map->mask0;				\
    /* Indexes need extra high bits? */		\
    if (resized) {				\
	Sort2(i1, i2);				\
	i1 |= fptag << map->logsize0;		\
	i2 = i1 ^ xorme;			\
	i1 &= map->mask1;			\
	i2 &= map->mask1;			\
    }						\
    struct fpmap_bent *b1, *b2;			\
    b1 = map->bb + i1 * bsize;			\
    b2 = map->bb + i2 * bsize

// Template for map->find virtual functions.
static inline size_t t_find(const struct fpmap *map, uint64_t fp, struct fpmap_bent *match[10],
	int bsize, bool resized, int nstash)
{
    dFP2IB;
    size_t n = 0;

    // Branches are predictable, no need for cmov.
#if 1
#define AddMatch(tag1, tag2, ent)	\
    do {				\
	if (unlikely(tag1 == tag2))	\
	    match[n++] = ent;		\
    } while (0)
#else
#define AddMatch(tag1, tag2, ent)	\
    do {				\
	match[n] = ent;			\
	n += (tag1 == tag2);		\
    } while (0)
#endif

    if (bsize > 0) AddMatch(fptag, b1[0].fptag, &b1[0]);
    if (bsize > 0) AddMatch(fptag, b2[0].fptag, &b2[0]);

    struct fpmap_bent *st = (void *) map->stash.bent; // const cast
    if (nstash > 0) AddMatch(fptag, st[0].fptag, &st[0]);
    if (nstash > 1) AddMatch(fptag, st[1].fptag, &st[1]);

    if (bsize > 1) AddMatch(fptag, b1[1].fptag, &b1[1]);
    if (bsize > 1) AddMatch(fptag, b2[1].fptag, &b2[1]);
    if (bsize > 2) AddMatch(fptag, b1[2].fptag, &b1[2]);
    if (bsize > 2) AddMatch(fptag, b2[2].fptag, &b2[2]);
    if (bsize > 3) AddMatch(fptag, b1[3].fptag, &b1[3]);
    if (bsize > 3) AddMatch(fptag, b2[3].fptag, &b2[3]);

    return n;
}

// Virtual functions for map->find.
#define MakeVFuncs(BS, RE, ST) \
    static FPMAP_FASTCALL size_t FPMAP_NAME(find##BS##re##RE##st##ST) \
	(FPMAP_pFP64, const struct fpmap *map, struct fpmap_bent *match[10]) \
    { return t_find(map, fp, match, BS, RE, ST); }
#define MakeAllVFuncs	\
    MakeVFuncs(2, 0, 0) MakeVFuncs(2, 0, 1) MakeVFuncs(2, 0, 2) \
    MakeVFuncs(3, 0, 0) MakeVFuncs(3, 0, 1) MakeVFuncs(3, 0, 2) \
    MakeVFuncs(4, 0, 0) MakeVFuncs(4, 0, 1) MakeVFuncs(4, 0, 2) \
    MakeVFuncs(3, 1, 0) MakeVFuncs(3, 1, 1) MakeVFuncs(3, 1, 2) \
    MakeVFuncs(4, 1, 0) MakeVFuncs(4, 1, 1) MakeVFuncs(4, 1, 2)
MakeAllVFuncs

static inline struct fpmap_bent *insert(struct fpmap_bent *b1, struct fpmap_bent *b2,
	int bsize)
{
    if (bsize > 0 && b1[0].fptag == 0) return &b1[0];
    if (bsize > 0 && b2[0].fptag == 0) return &b2[0];
    if (bsize > 1 && b1[1].fptag == 0) return &b1[1];
    if (bsize > 1 && b2[1].fptag == 0) return &b2[1];
    if (bsize > 2 && b1[2].fptag == 0) return &b1[2];
    if (bsize > 2 && b2[2].fptag == 0) return &b2[2];
    if (bsize > 3 && b1[3].fptag == 0) return &b1[3];
    if (bsize > 3 && b2[3].fptag == 0) return &b2[3];
    return NULL;
}

static inline bool kick(struct fpmap_bent be, struct fpmap_bent *bb, struct fpmap_bent *b,
	size_t i, struct fpmap_bent *obe, uint32_t *oi, int logsize, uint32_t mask, int bsize)
{
    int maxkick = 2 * logsize;
    do {
	// Put at the top, kick out from the bottom.
	// Using *obe as a temporary register.
	*obe = b[0];
	if (bsize > 1) b[0] = b[1];
	if (bsize > 2) b[1] = b[2];
	if (bsize > 3) b[2] = b[3];
	b[bsize-1] = be;
	// Ponder over the entry that's been kicked out.
	// Find out the alternative bucket.
	uint32_t xorme = obe->fptag;
#if FPMAP_FPTAG_BITS == 16
	xorme *= Golden32;
#endif
	i ^= xorme;
	i &= mask;
	b = bb + bsize * i;
	// Insert to the alternative bucket.
	if (bsize > 0 && b[0].fptag == 0) return b[0] = *obe, true;
	if (bsize > 1 && b[1].fptag == 0) return b[1] = *obe, true;
	if (bsize > 2 && b[2].fptag == 0) return b[2] = *obe, true;
	if (bsize > 3 && b[3].fptag == 0) return b[3] = *obe, true;
	be = *obe;
    } while (--maxkick >= 0);
    // Ran out of tries? obe already set.
    *oi = i;
    return false;
}

// TODO: aligned moves
#define A16(p) (p)

#define COPY2(dst, src) memcpy(dst, src, 2 * FPMAP_BENT_SIZE)
#define BE0 (struct fpmap_bent){ .fptag = 0 }

static inline struct fpmap_bent *resize23(struct fpmap_bent *bb, size_t nb)
{
    bb = realloc(bb, 3 * nb * sizeof BE0);
    if (!bb)
	return NULL;

    // Reinterpret as a 3-tier array.
    //
    //             2 3 . .   . . . .
    //   1 2 3 4   1 3 4 .   1 2 3 4
    //   1 2 3 4   1 2 4 .   1 2 3 4

    for (size_t i = nb - 2; i; i -= 2) {
	struct fpmap_bent *src0 = bb + 2 * i, *src1 = src0 + 2;
	struct fpmap_bent *dst0 = bb + 3 * i, *dst1 = dst0 + 3;
	COPY2(    dst1 , A16(src1)); dst1[2] = BE0;
	COPY2(A16(dst0), A16(src0)); dst0[2] = BE0;
    }
    bb[5] = BE0, bb[4] = bb[3], bb[3] = bb[2], bb[2] = BE0;
    return bb;
}

static inline struct fpmap_bent *resize34(struct fpmap_bent *bb, size_t nb)
{
    bb = realloc(bb, 4 * nb * sizeof BE0);
    if (!bb)
	return NULL;

    // Reinterpret as a 4-tier array.
    //
    //             2 3 4 .   . . . .
    //   1 2 3 4   1 3 4 .   1 2 3 4
    //   1 2 3 4   1 2 4 .   1 2 3 4
    //   1 2 3 4   1 2 3 .   1 2 3 4

    for (size_t i = nb - 2; i; i -= 2) {
	struct fpmap_bent *src0 = bb + 3 * i, *src1 = src0 + 3;
	struct fpmap_bent *dst0 = bb + 4 * i, *dst1 = dst0 + 4;
	dst1[2] = src1[2]; COPY2(A16(dst1),     src1 ); dst1[3] = BE0;
	dst0[2] = src0[2]; COPY2(A16(dst0), A16(src0)); dst0[3] = BE0;
    }
    bb[7] = BE0, bb[6] = bb[5], bb[5] = bb[4], bb[4] = bb[3], bb[3] = BE0;
    return bb;
}

// Template for map->insert virtual functions.
static inline struct fpmap_bent *t_insert(struct fpmap *map, uint64_t fp,
	int bsize, bool resized, int nstash)
{
    dFP2IB;
    struct fpmap_bent *be = insert(b1, b2, bsize);
    if (be) {
	be->fptag = fptag;
	return map->cnt++, be;
    }
    struct fpmap_bent kbe = { .fptag = fptag };
    int logsize = resized ? map->logsize1 : map->logsize0;
    uint32_t mask = resized ? map->mask1 : map->mask0;
    if (kick(kbe, map->bb, b1, i1, &kbe, &i1, logsize, mask, bsize))
	return map->cnt++, &b1[bsize-1];
    return NULL;
}

// Virtual functions for map->insert.
#undef MakeVFuncs
#define MakeVFuncs(BS, RE, ST) \
    static FPMAP_FASTCALL struct fpmap_bent *FPMAP_NAME(insert##BS##re##RE##st##ST) \
	(FPMAP_pFP64, struct fpmap *map) \
    { return t_insert(map, fp, BS, RE, ST); }
MakeAllVFuncs

#include <assert.h>
#include <errno.h>

struct fpmap *fpmap_new(int logsize)
{
    assert(logsize >= 0);
    if (logsize < 4)
	logsize = 4;
    if (logsize > 32)
	return errno = E2BIG, NULL;

    // Starting with two slots per bucket.
    size_t nb = (size_t) 1 << logsize;
    struct fpmap_bent *bb = calloc(nb, 2 * sizeof(struct fpmap_bent));
    if (!bb)
	return NULL;

    struct fpmap *map = malloc(sizeof *map);
    if (!map)
	return free(bb), NULL;

    map->find = FPMAP_NAME(find2re0st0);
    map->insert = FPMAP_NAME(insert2re0st0);
    map->bsize = 2;
    map->nstash = 0;
    map->logsize0 = map->logsize1 = logsize;
    map->mask0 = map->mask1 = nb - 1;
    map->bb = bb;

    return (struct fpmap *) map;
}

void fpmap_free(struct fpmap *arg)
{
    struct fpmap *map = (void *) arg;
    if (!map)
	return;
#ifdef FPMAP_DEBUG
    // The number of entries must match the occupied slots.
    size_t cnt = 0;
    struct fpmap_bent *bb = map->bb;
    size_t n = map->bsize * (map->mask1 + 1);
    for (size_t i = 0; i < n; i += 4)
	cnt += (bb[i+0].fptag > 0)
	    +  (bb[i+1].fptag > 0)
	    +  (bb[i+2].fptag > 0)
	    +  (bb[i+3].fptag > 0);
    assert(cnt == map->cnt);
#endif
    free(map->bb);
    free(map);
}
