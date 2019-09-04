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

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include "fp47map.h"

union bent {
    uint64_t copy8;
    struct {
	uint32_t fptag;
	uint32_t pos;
    };
};

#define BE0 (union bent){0}

struct stash {
    union bent be[2];
    // Since bucket entries are looked up by index+fptag, we also need to
    // remember the index (there are actually two symmetrical indices and
    // we may store either of them, or possibly the smaller one).
    uint32_t i1[2];
};

#define unlikely(cond) __builtin_expect(cond, 0)
#define likely(cond) __builtin_expect(!(cond), 0)

// The inline functions below rely heavily on constant propagation.
#define inline inline __attribute__((always_inline))

// 1 + fp % UINT32_MAX
static inline uint32_t mod32(uint64_t fp)
{
    uint32_t lo = fp;
    uint32_t hi = fp >> 32;
    lo += 1;
    if (unlikely(lo == 0))
	lo = 1;
    lo += hi;
    lo += (lo < hi);
    return lo;
}

// Fingerprint to indices.
// Note that the two buckets are completely symmetrical with regard to xor,
// i.e. the information about "the first and true" index is not preserved.
// This looses about 1 bit out of 32+logsize bits of hashing material.
#define dFP2I					\
    uint32_t i1 = fp >> 32;			\
    uint32_t fptag = mod32(fp);			\
    uint32_t i2 = i1 ^ fptag;			\
    i1 &= map->mask0;				\
    i2 &= map->mask0

// Indices to buckets.
#define dI2B					\
    if (resized) {				\
	/* Indices need extra high bits. */	\
	i1 = (i2 < i1) ? i2 : i1;		\
	i1 |= fptag << map->logsize0;		\
	i2 = i1 ^ fptag;			\
	i1 &= map->mask1;			\
	i2 &= map->mask1;			\
    }						\
    union bent *b1 = map->bb;			\
    union bent *b2 = map->bb;			\
    b1 += i1 * bsize;				\
    b2 += i2 * bsize

// Template for map->find virtual functions.
static inline unsigned t_find(const struct fp47map *map, uint64_t fp,
	uint32_t mpos[FP47MAP_pMAXFIND], int bsize, bool resized, int nstash)
{
    dFP2I;
    unsigned n = 0;
    // Check the stash first, unlikely to match.
    struct stash *st = (void *) map->stash;
    if (nstash > 0 && unlikely(st->be[0].fptag == fptag))
	if (likely(st->i1[0] == i1 || st->i1[0] == i2))
	    mpos[n++] = st->be[0].pos;
    if (nstash > 1 && unlikely(st->be[1].fptag == fptag))
	if (likely(st->i1[1] == i1 || st->i1[1] == i2))
	    mpos[n++] = st->be[1].pos;
    // Check the buckets.
    dI2B;
    if (bsize > 0 && b1[0].fptag == fptag) mpos[n++] = b1[0].pos;
    if (bsize > 0 && b2[0].fptag == fptag) mpos[n++] = b2[0].pos;
    if (bsize > 1 && b1[1].fptag == fptag) mpos[n++] = b1[1].pos;
    if (bsize > 1 && b2[1].fptag == fptag) mpos[n++] = b2[1].pos;
    if (bsize > 2 && b1[2].fptag == fptag) mpos[n++] = b1[2].pos;
    if (bsize > 2 && b2[2].fptag == fptag) mpos[n++] = b2[2].pos;
    if (bsize > 3 && b1[3].fptag == fptag) mpos[n++] = b1[3].pos;
    if (bsize > 3 && b2[3].fptag == fptag) mpos[n++] = b2[3].pos;
    return n;
}

static inline void t_prefetch(const struct fp47map *map, uint64_t fp,
      int bsize, int resized)
{
    dFP2I; dI2B;
    __builtin_prefetch(b1);
    __builtin_prefetch(b2);
}

// Virtual functions, prototypes for now.
#define MakeFindVFunc(BS, RE, ST) \
    static FP47MAP_FASTCALL unsigned fp47map_find##BS##re##RE##st##ST(FP47MAP_pFP64, \
	    const struct fp47map *map, uint32_t mpos[FP47MAP_pMAXFIND]);
#define MakeInsertVFunc(BS, RE) \
    static FP47MAP_FASTCALL int fp47map_insert##BS##re##RE(FP47MAP_pFP64, \
	    struct fp47map *map, uint32_t pos);
#define MakePrefetchVFunc(BS, RE) \
    static FP47MAP_FASTCALL void fp47map_prefetch##BS##re##RE(FP47MAP_pFP64, \
	    const struct fp47map *map);

// For the same bucket size, find and insert are placed back to back in memory.
// This should spare us some L1i cache misses.
#define MakeVFuncs(BS, RE) \
    MakePrefetchVFunc(BS, RE) \
    MakeFindVFunc(BS, RE, 0) \
    MakeInsertVFunc(BS, RE) \
    MakeFindVFunc(BS, RE, 1) \
    MakeFindVFunc(BS, RE, 2)

// There are no resized function for bsize == 2, becuase we first go 2->3->4,
// then double the number of buckets for 4->3 and then go 3->4 again.
#define MakeAllVFuncs \
    MakeVFuncs(2, 0) \
    MakeVFuncs(3, 0) MakeVFuncs(4, 0) \
    MakeVFuncs(3, 1) MakeVFuncs(4, 1)
MakeAllVFuncs

// When BS and RE are literals.
#define setVFuncsA(map, BS, RE, ST)			\
    map->find =						\
	(ST == 0) ? fp47map_find##BS##re##RE##st0 :	\
	(ST == 1) ? fp47map_find##BS##re##RE##st1 :	\
		    fp47map_find##BS##re##RE##st2 ,	\
    map->insert =   fp47map_insert##BS##re##RE    ,	\
    map->prefetch = fp47map_prefetch##BS##re##RE

static inline void setVFuncs(struct fp47map *map, int bsize, bool resized, int nstash)
{
    if (bsize == 2) {
	if (resized) assert(0);
	else         setVFuncsA(map, 2, 0, nstash);
    }
    if (bsize == 3) {
	if (resized) setVFuncsA(map, 3, 1, nstash);
	else         setVFuncsA(map, 3, 0, nstash);
    }
    if (bsize == 4) {
	if (resized) setVFuncsA(map, 4, 1, nstash);
	else         setVFuncsA(map, 4, 0, nstash);
    }
}

// Sentinels at the end of map->bb, for fp47map_next.
#define SENTINELS 3

struct fp47map *fp47map_new(int logsize)
{
    assert(logsize >= 0);
    if (logsize < 4)
	logsize = 4;
    // The ultimate limit imposed by the hashing scheme is 2^32 buckets.
    if (logsize > 32)
	return errno = E2BIG, NULL;
    // The limit on 32-bit platforms is 2GB, logsize=28 allocates 4GB.
    if (logsize > 27 && sizeof(size_t) < 5)
	return errno = ENOMEM, NULL;

    // Starting with two slots per bucket.
    size_t nb = (size_t) 1 << logsize;
    size_t nbe = 2 * nb;
    union bent *bb = calloc(nbe + SENTINELS, sizeof BE0);
    if (!bb)
	return NULL;

    struct fp47map *map = malloc(sizeof *map);
    if (!map)
	return free(bb), NULL;

    for (unsigned i = 0; i < SENTINELS; i++)
	bb[nbe+i].fptag = UINT32_MAX;

    map->bb = bb;
    map->cnt = 0;
    map->bsize = 2;
    map->nstash = 0;
    map->logsize0 = map->logsize1 = logsize;
    map->mask0 = map->mask1 = nb - 1;

    setVFuncs(map, 2, 0, 0);
    return map;
}

void fp47map_free(struct fp47map *map)
{
    if (!map)
	return;
#ifdef FP47MAP_DEBUG
    // The number of entries must match the occupied slots.
    size_t cnt = 0;
    union bent *bb = map->bb;
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

uint32_t *FP47MAP_FASTCALL fp47map_next(const struct fp47map *map, size_t *iter)
{
    size_t i = *iter;
    size_t n = map->bsize * (map->mask1 + (size_t) 1);
    union bent *bb = map->bb;
    while (bb[i].fptag == 0)
	i++;
    if (i < n)
	return *iter = i + 1, &bb[i].pos;
    struct stash *st = (void *) &map->stash;
    if (map->nstash == 0)
	return *iter = 0, NULL;
    if (i == n)
	return *iter = n + 1, &st->be[0].pos;
    if (map->nstash == 1)
	return *iter = 0, NULL;
    if (i == n + 1)
	return *iter = n + 2, &st->be[1].pos;
    return *iter = 0, NULL;
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
#undef MakeFind32VFunc1_st0
#define MakeFind32VFunc1_st0(BS, RE) \
    static FPMAP_FASTCALL size_t FPMAP_NAME(find32##BS##re##RE##st0) \
	(FPMAP_pFP64, const struct fpmap *map, \
	 uint32_t match[FPMAP_pMAXFIND]) \
    { return t_find32(map, fp, match, BS, RE, 0, 0); }
#undef MakeFindVFunc1_st1
#define MakeFindVFunc1_st1(BS, RE, ST, FA) \
    static FPMAP_FASTCALL size_t FPMAP_NAME(find##BS##re##RE##st##ST##fa##FA) \
	(FPMAP_pFP64, const struct fpmap *map, \
	 struct fpmap_bent *match[FPMAP_pMAXFIND]) \
    { return t_find(map, fp, match, BS, RE, ST, FA); }
#undef MakeFind32VFunc1_st1
#define MakeFind32VFunc1_st1(BS, RE, ST, FA) \
    static FPMAP_FASTCALL size_t FPMAP_NAME(find32##BS##re##RE##st##ST##fa##FA) \
	(FPMAP_pFP64, const struct fpmap *map, \
	 uint32_t match[FPMAP_pMAXFIND]) \
    { return t_find32(map, fp, match, BS, RE, ST, FA); }
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
