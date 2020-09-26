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

#include "fp47m.h"

struct stash {
    union bent be[2];
    // Since bucket entries are looked up by index+tag, we also need to
    // remember the index (there are actually two symmetrical indices and
    // we may store either of them, or possibly the smaller one).
    uint32_t i1[2];
};

// Indices to buckets.
#define dI2B					\
    if (resized)				\
	ResizeI;				\
    union bent *b1 = map->bb;			\
    union bent *b2 = map->bb;			\
    b1 += i1 * bsize;				\
    b2 += i2 * bsize

// Template for map->find virtual functions.
static inline unsigned t_find(const struct fp47map *map, uint64_t fp,
	uint32_t *mpos, int bsize, bool resized, int nstash)
{
    dFP2I;
    unsigned n = 0;
    // Check the stash first, unlikely to match.
    struct stash *st = (void *) map->stash;
    if (nstash > 0 && unlikely(st->be[0].tag == tag))
	if (likely(st->i1[0] == i1 || st->i1[0] == i2))
	    mpos[n++] = st->be[0].pos;
    if (nstash > 1 && unlikely(st->be[1].tag == tag))
	if (likely(st->i1[1] == i1 || st->i1[1] == i2))
	    mpos[n++] = st->be[1].pos;
    // Check the buckets.
    dI2B;
    if (bsize > 0 && b1[0].tag == tag) mpos[n++] = b1[0].pos;
    if (bsize > 0 && b2[0].tag == tag) mpos[n++] = b2[0].pos;
    if (bsize > 1 && b1[1].tag == tag) mpos[n++] = b1[1].pos;
    if (bsize > 1 && b2[1].tag == tag) mpos[n++] = b2[1].pos;
    if (bsize > 2 && b1[2].tag == tag) mpos[n++] = b1[2].pos;
    if (bsize > 2 && b2[2].tag == tag) mpos[n++] = b2[2].pos;
    if (bsize > 3 && b1[3].tag == tag) mpos[n++] = b1[3].pos;
    if (bsize > 3 && b2[3].tag == tag) mpos[n++] = b2[3].pos;
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
    static FASTCALL unsigned fp47map_find##BS##re##RE##st##ST(uint64_t fp, \
	    const struct fp47map *map, uint32_t *mpos);
#define MakeInsertVFunc(BS, RE) \
    static FASTCALL int fp47map_insert##BS##re##RE(uint64_t fp, \
	    struct fp47map *map, uint32_t pos);
#define MakePrefetchVFunc(BS, RE) \
    static FASTCALL void fp47map_prefetch##BS##re##RE(uint64_t fp, \
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
    union bent *bb = calloc(nbe, sizeof BE0);
    if (!bb)
	return NULL;

    struct fp47map *map = malloc(sizeof *map);
    if (!map)
	return free(bb), NULL;

    map->bb = bb;
    map->cnt = 0;
    map->bsize = 2;
    map->nstash = 0;
    map->logsize0 = map->logsize1 = logsize;
    map->mask0 = map->mask1 = nb - 1;
    map->bboff = 0;

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
	cnt += (bb[i+0].tag > 0)
	    +  (bb[i+1].tag > 0)
	    +  (bb[i+2].tag > 0)
	    +  (bb[i+3].tag > 0);
    assert(cnt == map->cnt);
#endif
    free(map->bb - map->bboff);
    free(map);
}

static inline union bent *empty2(union bent *b1, union bent *b2, int bsize)
{
    if (bsize > 0 && b1[0].tag == 0) return &b1[0];
    if (bsize > 0 && b2[0].tag == 0) return &b2[0];
    if (bsize > 1 && b1[1].tag == 0) return &b1[1];
    if (bsize > 1 && b2[1].tag == 0) return &b2[1];
    if (bsize > 2 && b1[2].tag == 0) return &b1[2];
    if (bsize > 2 && b2[2].tag == 0) return &b2[2];
    if (bsize > 3 && b1[3].tag == 0) return &b1[3];
    if (bsize > 3 && b2[3].tag == 0) return &b2[3];
    return NULL;
}

static inline bool kickloop(union bent *bb, union bent *b1,
	union bent be, uint32_t i1, union bent *obe, uint32_t *oi1,
	int maxkick, uint32_t mask, int bsize)
{
    do {
	// Put at the top, kick out from the bottom.
	// Using *obe as a temporary register.
	*obe = b1[0];
	if (bsize > 1) b1[0] = b1[1];
	if (bsize > 2) b1[1] = b1[2];
	if (bsize > 3) b1[2] = b1[3];
	b1[bsize-1] = be;
	// Ponder over the entry that's been kicked out.
	// Find out the alternative bucket.
	i1 ^= obe->tag;
	i1 &= mask;
	b1 = bb, b1 += i1 * bsize;
	// Insert to the alternative bucket.
	if (bsize > 0 && b1[0].tag == 0) return b1[0] = *obe, true;
	if (bsize > 1 && b1[1].tag == 0) return b1[1] = *obe, true;
	if (bsize > 2 && b1[2].tag == 0) return b1[2] = *obe, true;
	if (bsize > 3 && b1[3].tag == 0) return b1[3] = *obe, true;
	be = *obe;
    } while (--maxkick >= 0);
    // Ran out of tries? obe already set, recover oi1.
    i1 ^= be.tag;
    i1 &= mask;
    *oi1 = i1;
    return false;
}

// When kickloop fails, we may need to revert the buckets to the original
// state.  So we just insert the kicked-out entry in the reverse direction.
static inline uint32_t kickback(union bent *bb, union bent be, uint32_t i1,
	int maxkick, uint32_t mask, int bsize)
{
    do {
	union bent *b1 = bb; b1 += i1 * bsize;
	union bent obe = b1[bsize-1];
	if (bsize > 3) b1[3] = b1[2];
	if (bsize > 2) b1[2] = b1[1];
	if (bsize > 1) b1[1] = b1[0];
	b1[0] = be;
	i1 ^= obe.tag;
	i1 &= mask;
	be = obe;
    } while (--maxkick >= 0);
    return be.tag;
}

// TODO: aligned moves
#define A16(p) (p)

#define COPY2(dst, src) memcpy(dst, src, 2 * sizeof BE0)

static inline void reinterp23(union bent *bb, size_t nb)
{
    // Reinterpret as a 3-tier array.
    //
    //             2 3 . .   . . . .
    //   1 2 3 4   1 3 4 .   1 2 3 4
    //   1 2 3 4   1 2 4 .   1 2 3 4

    for (size_t i = nb - 2; i; i -= 2) {
	union bent *src0 = bb + 2 * i, *src1 = src0 + 2;
	union bent *dst0 = bb + 3 * i, *dst1 = dst0 + 3;
	COPY2(    dst1 , A16(src1)); dst1[2] = BE0;
	COPY2(A16(dst0), A16(src0)); dst0[2] = BE0;
    }
    bb[5] = BE0, bb[4] = bb[3], bb[3] = bb[2], bb[2] = BE0;
}

static inline void reinterp34(union bent *bb, size_t nb)
{
    // Reinterpret as a 4-tier array.
    //
    //             2 3 4 .   . . . .
    //   1 2 3 4   1 3 4 .   1 2 3 4
    //   1 2 3 4   1 2 4 .   1 2 3 4
    //   1 2 3 4   1 2 3 .   1 2 3 4

    for (size_t i = nb - 2; i; i -= 2) {
	union bent *src0 = bb + 3 * i, *src1 = src0 + 3;
	union bent *dst0 = bb + 4 * i, *dst1 = dst0 + 4;
	dst1[2] = src1[2]; COPY2(A16(dst1),     src1 ); dst1[3] = BE0;
	dst0[2] = src0[2]; COPY2(A16(dst0), A16(src0)); dst0[3] = BE0;
    }
    bb[7] = BE0, bb[6] = bb[5], bb[5] = bb[4], bb[4] = bb[3], bb[3] = BE0;
}

// After the table gets resized, we try to reinsert the stashed elements.
static inline unsigned restash2(struct fp47map *map,
	int maxkick, uint32_t mask, int bsize, bool resized)
{
    unsigned k = 0;
    struct stash *st = (void *) &map->stash;
    for (unsigned j = 0; j < 2; j++) {
	uint32_t tag = st->be[j].tag;
	uint32_t i1 = st->i1[j];
	uint32_t i2 = i1 ^ tag;
	i2 &= map->mask0;
	dI2B;
	union bent *be = empty2(b1, b2, bsize);
	if (be) {
	    *be = st->be[j];
	    continue;
	}
	if (kickloop(map->bb, b1, st->be[j], i1, &st->be[k], &i1, maxkick, mask, bsize))
	    continue;
	// An entry from i1 has landed into st->be[k].
	st->i1[k++] = i1;
    }
    return k;
}

static inline int t_insert(struct fp47map *map, uint64_t fp, uint32_t pos,
	int bsize, bool resized)
{
    dFP2I; dI2B;
    map->cnt++; // strategical bump, may renege
    union bent *be = empty2(b1, b2, bsize);
    if (be) {
	be->tag = tag, be->pos = pos;
	return 1;
    }
    union bent kbe = { .tag = tag, .pos = pos };
    int maxkick = 2 * (resized ? map->logsize1 : map->logsize0);
    uint32_t mask = resized ? map->mask1 : map->mask0;
    if (kickloop(map->bb, b1, kbe, i1, &kbe, &i1, maxkick, mask, bsize))
	return 1;
    if (map->nstash < 2) {
	struct stash *st = (void *) &map->stash;
	st->be[map->nstash] = kbe;
	st->i1[map->nstash] = i1;
	map->nstash++, map->cnt--;
	setVFuncs(map, bsize, resized, map->nstash);
	return 1;
    }
    // The 4->3 scenario is the "true resize", quite complex.
    if (bsize == 4) {
	uint32_t fpout = kickback(map->bb, kbe, i1, maxkick, mask, bsize);
	assert(fpout == tag);
	return insert4tail(map, fp, pos);
    }
    // With 2->3 and 3->4 though, we just extend the buckets.
    size_t nb = mask + (size_t) 1;
    size_t nbe = (bsize + 1) * nb;
    union bent *bb = realloc(map->bb, nbe * sizeof BE0);
    if (!bb) {
	uint32_t fpout = kickback(map->bb, kbe, i1, maxkick, mask, bsize);
	assert(fpout == tag);
	map->cnt--;
	return -1;
    }
    if (bsize == 2) reinterp23(bb, nb);
    if (bsize == 3) reinterp34(bb, nb);
    map->bb = bb;
    map->bsize = bsize + 1;
    b1 = bb + i1 * (bsize + 1);
    // Insert kbe at i1, no kicks required.
    b1[bsize] = kbe;
    // Reinsert the stashed elements.
    map->cnt += map->nstash;
    map->nstash = restash2(map, maxkick, mask, bsize + 1, resized);
    map->cnt -= map->nstash;
    setVFuncs(map, bsize + 1, resized, map->nstash);
    return 2;
}

// Finally instatntiate virtual functions.
#undef MakeFindVFunc
#define MakeFindVFunc(BS, RE, ST) \
    static FASTCALL unsigned fp47map_find##BS##re##RE##st##ST(uint64_t fp, \
	    const struct fp47map *map, uint32_t *mpos) \
    { return t_find(map, fp, mpos, BS, RE, ST); }
#undef MakeInsertVFunc
#define MakeInsertVFunc(BS, RE) \
    static FASTCALL int fp47map_insert##BS##re##RE(uint64_t fp, \
	    struct fp47map *map, uint32_t pos) \
    { return t_insert(map, fp, pos, BS, RE); }
#undef MakePrefetchVFunc
#define MakePrefetchVFunc(BS, RE) \
    static FASTCALL void fp47map_prefetch##BS##re##RE(uint64_t fp, \
	    const struct fp47map *map) \
    { t_prefetch(map, fp, BS, RE); }
MakeAllVFuncs
