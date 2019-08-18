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

// Fingerprint to indices.
// Note that the two buckets are completely symmetrical with regard to xor,
// i.e. the information about "the first and true" index is not preserved.
// This looses about 1 bit out of 32+logsize bits of hashing material.
#if FPMAP_FPTAG_BITS == 16
#define dFP2I					\
    uint32_t i1 = fp >> 32;			\
    uint32_t fptag = mod16(fp);			\
    uint32_t xorme = fptag * Golden32;		\
    uint32_t i2 = i1 ^ xorme;			\
    i1 &= map->mask0;				\
    i2 &= map->mask0
#else
#define dFP2I					\
    uint32_t i1 = fp;				\
    uint32_t fptag = mod32(fp);			\
    uint32_t xorme = fptag;			\
    uint32_t i2 = i1 ^ xorme;			\
    i1 &= map->mask0;				\
    i2 &= map->mask0
#endif

// Fingerprint to indices and buckets.
#define dFP2IB					\
    dFP2I;					\
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

#if FPMAP_REG64
#define uintREG_t uint64_t
#else
#define uintREG_t uint32_t
#endif

// Template for map->find virtual functions.
static inline size_t t_find(const struct fpmap *map, uint64_t fp,
	struct fpmap_bent *match[FPMAP_pMAXFIND],
	int bsize, bool resized, int nstash, bool faststash)
{
    dFP2I;
    if (resized || nstash)
	Sort2(i1, i2);

    // How the stash is to be checked.
    uintREG_t stlo;
    assert(nstash >= faststash);
    // If we are resized, then the calculation of stlo is the same as adding
    // extra fptag high bits to i1, so we get stlo for free.
    if (resized && faststash) {
	stlo = i1 | (uintREG_t) fptag << map->logsize0;
	i1 = stlo;
	i2 = i1 ^ xorme;
	i1 &= map->mask1;
	i2 &= map->mask1;
    }
    else if (resized) {
	// No need for stlo, will check i1+fptag.
	i1 |= fptag << map->logsize0;
	i2 = i1 ^ xorme;
	i1 &= map->mask1;
	i2 &= map->mask1;
    }
    else if (faststash) {
	// On 64-bit platforms, there are enough bits to combine i1+fptag
	// and even avoid referencing map->logsize0.  On 32-bit platforms,
	// we only use faststash with 16-bit fptag and logsize0 <= 16.
	stlo = i1 | (uintREG_t) fptag << sizeof(uintREG_t) * 4;
	// Not resized, no extra bits for i1 and i2.
    }

    size_t n = 0;
    struct fpmap_bent *b1 = map->bb + i1 * bsize;
    struct fpmap_bent *b2 = map->bb + i2 * bsize;

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

    // This can hopefully issue two memory loads in parallel.
    if (bsize > 0) AddMatch(fptag, b1[0].fptag, &b1[0]);
    if (bsize > 0) AddMatch(fptag, b2[0].fptag, &b2[0]);

    struct fpmap_stash *st = (void *) &map->stash; // const cast
#define AddStash(j)			\
    do {				\
	if (st->be[j].fptag == fptag && st->lo[j] == i1) \
	    match[n++] = &st->be[j];	\
    } while (0)

    if (faststash) {
	if (nstash > 0) AddMatch(stlo, st->lo[0], &st->be[0]);
	if (nstash > 1) AddMatch(stlo, st->lo[1], &st->be[1]);
    }
    else if (nstash) {
	if (nstash > 0) AddStash(0);
	if (nstash > 1) AddStash(1);
    }

    if (bsize > 1) AddMatch(fptag, b1[1].fptag, &b1[1]);
    if (bsize > 1) AddMatch(fptag, b2[1].fptag, &b2[1]);
    if (bsize > 2) AddMatch(fptag, b1[2].fptag, &b1[2]);
    if (bsize > 2) AddMatch(fptag, b2[2].fptag, &b2[2]);
    if (bsize > 3) AddMatch(fptag, b1[3].fptag, &b1[3]);
    if (bsize > 3) AddMatch(fptag, b2[3].fptag, &b2[3]);

    return n;
}

static inline void t_prefetch(const struct fpmap *map, uint64_t fp,
      int bsize, int resized)
{
    dFP2IB;
    __builtin_prefetch(&b1[0].fptag);
    __builtin_prefetch(&b2[0].fptag);
}

// Virtual functions, prototypes for now.
#define MakeFindVFunc1_st0(BS, RE) \
    static FPMAP_FASTCALL size_t FPMAP_NAME(find##BS##re##RE##st0) \
	(FPMAP_pFP64, const struct fpmap *map, \
	 struct fpmap_bent *match[FPMAP_pMAXFIND]);
#define MakeFindVFunc1_st1(BS, RE, ST, FA) \
    static FPMAP_FASTCALL size_t FPMAP_NAME(find##BS##re##RE##st##ST##fa##FA) \
	(FPMAP_pFP64, const struct fpmap *map, \
	 struct fpmap_bent *match[FPMAP_pMAXFIND]);
#define MakeInsertVFunc1(BS, RE) \
    static FPMAP_FASTCALL struct fpmap_bent *FPMAP_NAME(insert##BS##re##RE) \
	(FPMAP_pFP64, struct fpmap *map);
#define MakePrefetchVFunc1(BS, RE) \
    static FPMAP_FASTCALL void FPMAP_NAME(prefetch##BS##re##RE) \
	(FPMAP_pFP64, const struct fpmap *map);

// On 64-bit platforms, the check for index+fptag is always fused
// (the faststash mode is always on).
#if FPMAP_REG64
#define MakeFindVFuncs_st1(BS, RE) \
    MakeFindVFunc1_st1(BS, RE, 1, 1) \
    MakeFindVFunc1_st1(BS, RE, 2, 1)
#define SelectStashVFunc(logsize0, fa0, fa1) fa1
// On 32-bit platforms with 32-bit fptag, the fassthash mode is always off.
#elif FPMAP_BENT_SIZE == 32
#define MakeFindVFuncs_st1(BS, RE) \
    MakeFindVFunc1_st1(BS, RE, 1, 0) \
    MakeFindVFunc1_st1(BS, RE, 2, 0)
#define SelectStashVFunc(logsize0, fa0, fa1) fa0
// Otherwise, there will be a runtime check.
#else
#define MakeFindVFuncs_st1(BS, RE) \
    MakeFindVFunc1_st1(BS, RE, 1, 0) \
    MakeFindVFunc1_st1(BS, RE, 2, 0) \
    MakeFindVFunc1_st1(BS, RE, 1, 1) \
    MakeFindVFunc1_st1(BS, RE, 2, 1)
#define SelectStashVFunc(logsize0, fa0, fa1) (logsize0 <= 16 ? fa1 : fa0)
#endif

// For the same bucket size, find and insert are placed back to back in memory.
// This should spare us some L1i cache misses.
#define MakeVFuncs(BS, RE) \
    MakeFindVFunc1_st0(BS, RE) \
    MakeInsertVFunc1(BS, RE) \
    MakePrefetchVFunc1(BS, RE) \
    MakeFindVFuncs_st1(BS, RE)

// There are no resized function for bsize == 2, becuase we first go 2->3->4,
// then double the number of buckets and go 3->4 again.
#define MakeAllVFuncs \
    MakeVFuncs(2, 0) \
    MakeVFuncs(3, 0) MakeVFuncs(4, 0) \
    MakeVFuncs(3, 1) MakeVFuncs(4, 1)
MakeAllVFuncs

// When BS and RE are literals.
#define setVFuncsBR(map, BS, RE, nstash, logsize0)	\
do {							\
    map->insert = FPMAP_NAME(insert##BS##re##RE);	\
    if (nstash == 0)					\
	map->find = FPMAP_NAME(find##BS##re##RE##st0);	\
    else if (nstash == 1)				\
	map->find = SelectStashVFunc(logsize0,		\
	    FPMAP_NAME(find##BS##re##RE##st1fa0),	\
	    FPMAP_NAME(find##BS##re##RE##st1fa1));	\
    else						\
	map->find = SelectStashVFunc(logsize0,		\
	    FPMAP_NAME(find##BS##re##RE##st2fa0),	\
	    FPMAP_NAME(find##BS##re##RE##st2fa1));	\
    map->prefetch = FPMAP_NAME(prefetch##BS##re##RE);	\
} while (0)

static inline void setVFuncs(struct fpmap *map, int bsize, bool resized, int nstash)
{
    if (bsize == 2) {
	if (resized)
	    assert(0);
	else
	    setVFuncsBR(map, 2, 0, nstash, map->logsize0);
    }
    else if (bsize == 3) {
	if (resized)
	    setVFuncsBR(map, 3, 1, nstash, map->logsize0);
	else
	    setVFuncsBR(map, 3, 0, nstash, map->logsize0);
    }
    else {
	if (resized)
	    setVFuncsBR(map, 4, 1, nstash, map->logsize0);
	else
	    setVFuncsBR(map, 4, 0, nstash, map->logsize0);
    }
}

static inline struct fpmap_bent *empty2(struct fpmap_bent *b1, struct fpmap_bent *b2,
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

static inline bool kickloop(struct fpmap_bent *bb,
	struct fpmap_bent be, struct fpmap_bent *b, uint32_t i,
	struct fpmap_bent *obe, struct fpmap_bent **ob, uint32_t *oi,
	int logsize, uint32_t mask, int bsize)
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
	if (FPMAP_FPTAG_BITS == 16)
	    xorme *= Golden32;
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
    *ob = b, *oi = i;
    return false;
}

// When kickloop fails, we may need to revert the buckets to the original
// state.  So we just insert the kicked-out entry in the reverse direction.
static inline uint32_t kickback(struct fpmap_bent *bb,
	struct fpmap_bent be, struct fpmap_bent *b, uint32_t i,
	int logsize, uint32_t mask, int bsize)
{
    int maxkick = 2 * logsize;
    do {
	struct fpmap_bent obe = b[bsize-1];
	if (bsize > 3) b[3] = b[2];
	if (bsize > 2) b[2] = b[1];
	if (bsize > 1) b[1] = b[0];
	b[0] = be;
	uint32_t xorme = obe.fptag;
	if (FPMAP_FPTAG_BITS == 16)
	    xorme *= Golden32;
	i ^= xorme;
	i &= mask;
	b = bb + bsize * i;
	be = obe;
    } while (--maxkick >= 0);
    return be.fptag;
}

// TODO: aligned moves
#define A16(p) (p)

#define COPY2(dst, src) memcpy(dst, src, 2 * FPMAP_BENT_SIZE)
#define BE0 (struct fpmap_bent){ .fptag = 0 }

static inline void reinterp23(struct fpmap_bent *bb, size_t nb)
{
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
}

static inline void reinterp34(struct fpmap_bent *bb, size_t nb)
{
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
}

// When an entry is about to enter the stash, we need to calculate stash.lo
// based the entry's index.  This must match to what t_find does.
static inline uintREG_t stash1lo(struct fpmap *map, struct fpmap_bent *be,
	uint32_t i1, bool resized)
{
    uint32_t fptag = be->fptag;
    uint32_t xorme = fptag;
    if (FPMAP_FPTAG_BITS == 16)
	xorme *= Golden32;
    uint32_t i2 = i1 ^ xorme;
    if (resized)
	i1 &= map->mask0;
    i2 &= map->mask0;
    i1 = (i2 < i1) ? i2 : i1;
    bool faststash = FPMAP_REG64 || (FPMAP_FPTAG_BITS == 16 && map->logsize0 <= 16);
    if (resized && faststash)
	return i1 | (uintREG_t) fptag << map->logsize0;
    if (resized)
	return i1;
    if (faststash)
	return i1 | (uintREG_t) fptag << sizeof(uintREG_t) * 4;
    return i1;
}

// After the table gets resized, we try to reinsert the stashed elements.
static inline unsigned restash2(struct fpmap *map, int bsize, bool resized)
{
    unsigned k = 0;
    struct fpmap_stash *st = &map->stash;
    for (unsigned j = 0; j < 2; j++) {
	uint32_t fptag = st->be[j].fptag;
	uint32_t xorme = fptag;
	if (FPMAP_FPTAG_BITS == 16)
	    xorme *= Golden32;
	uint32_t i1 = st->lo[j];
	uint32_t i2 = i1 ^ xorme;
	i1 &= map->mask0;
	i2 &= map->mask0;
	// No need to sort, i1 is already "lo".
	if (resized) {
	    i1 |= fptag << map->logsize0;
	    i2 = i1 ^ xorme;
	    i1 &= map->mask1;
	    i2 &= map->mask1;
	}
	struct fpmap_bent *b1 = map->bb + i1 * bsize;
	struct fpmap_bent *b2 = map->bb + i2 * bsize;
	struct fpmap_bent *be = empty2(b1, b2, bsize);
	if (be) {
	    *be = st->be[j];
	    continue;
	}
	int logsize = resized ? map->logsize1 : map->logsize0;
	uint32_t mask = resized ? map->mask1 : map->mask0;
	if (kickloop(map->bb, st->be[j], b1, i1, &st->be[k], &b2, &i2, logsize, mask, bsize))
	    continue;
	// An entry from i2 has landed into st->be[k].
	st->lo[k] = stash1lo(map, &st->be[k], i2, resized);
	k++;
    }
    return k;
}

// Sentinels at the end of map->bb, for fpmap_next.
#define SENTINELS 3

// Template for map->insert virtual functions.
static inline struct fpmap_bent *t_insert(struct fpmap *map, uint64_t fp,
	int bsize, bool resized)
{
    dFP2IB;
    map->cnt++; // strategical bump, may renege
    struct fpmap_bent *be = empty2(b1, b2, bsize);
    if (be) {
	be->fptag = fptag;
	return be;
    }
    struct fpmap_bent kbe = { .fptag = fptag };
    int logsize = resized ? map->logsize1 : map->logsize0;
    uint32_t mask = resized ? map->mask1 : map->mask0;
    if (kickloop(map->bb, kbe, b1, i1, &kbe, &b2, &i2, logsize, mask, bsize))
	return &b1[bsize-1];
    if (map->nstash < 2) {
	struct fpmap_stash *st = &map->stash;
	st->be[map->nstash] = kbe;
	st->lo[map->nstash] = stash1lo(map, &st->be[map->nstash], i2, resized);
	map->nstash++, map->cnt--;
	setVFuncs(map, bsize, resized, map->nstash);
	return &b1[bsize-1];
    }
    // The 4->3 scenario is the "true resize", quite complex.
    if (bsize == 4) {
	uint32_t fpout = kickback(map->bb, kbe, b2, i2, logsize, mask, bsize);
	assert(fpout == fptag);
	return insert4tail(map, fp);
    }
    // With 2->3 and 3->4 though, we just extend the buckets.
    size_t nb = mask + (size_t) 1;
    size_t nbe = (bsize + 1) * nb;
    struct fpmap_bent *bb = realloc(map->bb, (nbe + SENTINELS) * sizeof BE0);
    if (!bb) {
	uint32_t fpout = kickback(map->bb, kbe, b2, i2, logsize, mask, bsize);
	assert(fpout == fptag);
	map->cnt--;
	return NULL;
    }
    for (unsigned i = 0; i < SENTINELS; i++)
	bb[nbe+i].fptag = 1;
    if (bsize == 2) reinterp23(bb, nb);
    if (bsize == 3) reinterp34(bb, nb);
    map->bb = bb;
    map->bsize = bsize + 1;
    b1 = bb + (bsize + 1) * i1;
    b2 = bb + (bsize + 1) * i2;
    // Insert kbe at i2, no kicks required.
    b2[bsize] = kbe;
    // Reinsert the stashed elements.
    map->cnt += map->nstash;
    map->nstash = restash2(map, bsize + 1, resized);
    map->cnt -= map->nstash;
    setVFuncs(map, bsize + 1, resized, map->nstash);
    return &b1[bsize-1];
}

// Finally instatntiate virtual functions.
#undef MakeFindVFunc1_st0
#define MakeFindVFunc1_st0(BS, RE) \
    static FPMAP_FASTCALL size_t FPMAP_NAME(find##BS##re##RE##st0) \
	(FPMAP_pFP64, const struct fpmap *map, \
	 struct fpmap_bent *match[FPMAP_pMAXFIND]) \
    { return t_find(map, fp, match, BS, RE, 0, 0); }
#undef MakeFindVFunc1_st1
#define MakeFindVFunc1_st1(BS, RE, ST, FA) \
    static FPMAP_FASTCALL size_t FPMAP_NAME(find##BS##re##RE##st##ST##fa##FA) \
	(FPMAP_pFP64, const struct fpmap *map, \
	 struct fpmap_bent *match[FPMAP_pMAXFIND]) \
    { return t_find(map, fp, match, BS, RE, ST, FA); }
#undef MakeInsertVFunc1
#define MakeInsertVFunc1(BS, RE) \
    static FPMAP_FASTCALL struct fpmap_bent *FPMAP_NAME(insert##BS##re##RE) \
	(FPMAP_pFP64, struct fpmap *map) \
    { return t_insert(map, fp, BS, RE); }
#undef MakePrefetchVFunc1
#define MakePrefetchVFunc1(BS, RE) \
    static FPMAP_FASTCALL void FPMAP_NAME(prefetch##BS##re##RE) \
	(FPMAP_pFP64, const struct fpmap *map) \
    { t_prefetch(map, fp, BS, RE); }
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
    size_t nbe = 2 * nb;
    struct fpmap_bent *bb = calloc(nbe + SENTINELS, sizeof BE0);
    if (!bb)
	return NULL;

    struct fpmap *map = malloc(sizeof *map);
    if (!map)
	return free(bb), NULL;

    for (unsigned i = 0; i < SENTINELS; i++)
	bb[nbe+i].fptag = 1;

    map->bb = bb;
    map->cnt = 0;
    map->bsize = 2;
    map->nstash = 0;
    map->logsize0 = map->logsize1 = logsize;
    map->mask0 = map->mask1 = nb - 1;

    setVFuncs(map, 2, 0, 0);
    return map;
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
    size_t n = map->bsize * (map->mask1 + (size_t) 1);
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

struct fpmap_bent *FPMAP_FASTCALL fpmap_next(const struct fpmap *map,
					     size_t *iter)
{
    size_t i = *iter;
    struct fpmap_bent *bb = map->bb;
    size_t n = map->bsize * (map->mask1 + (size_t) 1);
    while (bb[i].fptag == 0)
	i++;
    if (i < n)
	return *iter = i + 1, &bb[i];
    struct fpmap_stash *st = (void *) &map->stash; // const cast
    if (map->nstash == 0)
	return *iter = 0, NULL;
    if (i == n)
	return *iter = n + 1, &st->be[0];
    if (map->nstash == 1)
	return *iter = 0, NULL;
    if (i == n + 1)
	return *iter = n + 2, &st->be[1];
    return *iter = 0, NULL;
}
