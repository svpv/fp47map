// Copyright (c) 2017, 2018, 2019, 2020 Alexey Tourbin
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

#if UINTPTR_MAX > UINT32_MAX
#define MALIGN 16
#elif defined(__GLIBC_PREREQ)
#if defined(__i386__) && __GLIBC_PREREQ(2, 26)
#define MALIGN 16
#else
#define MALIGN 8
#endif
#else // assume musl or jemalloc
#define MALIGN 16
#endif

struct fp47map *fp47map_new(int logsize)
{
    assert(logsize >= 0);
    if (logsize < 4)
	logsize = 4;
    // The ultimate limit imposed by the hashing scheme is 2^32 buckets.
    // The limit on 32-bit platforms is 2GB, logsize=28 allocates 4GB.
    if (logsize > ((sizeof(size_t) < 5) ? 27 : 32))
	return NULL;

    struct fp47map *map = aligned_alloc(16, sizeof *map);
    if (!map)
	return NULL;

    size_t nb = (size_t) 1 << logsize;
    size_t bytes = 16 * nb;
    void *bb;
    if (bytes >= MTHRESH) {
	bb = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (bb == MAP_FAILED)
	    return free(map), NULL;
    }
    else if (MALIGN >= 16) {
	bb = calloc(nb, 16);
	if (!bb)
	    return free(map), NULL;
	assert((uintptr_t) bb % 16 == 0);
    }
    else {
	bb = aligned_alloc(16, bytes);
	if (!bb)
	    return free(map), NULL;
	memset(bb, 0, 16 * nb);
    }

    map->bb = bb;
    map->cnt = 0;
    map->bsize = 2;
    map->nstash = 0;
    map->logsize0 = map->logsize1 = logsize;
    map->mask0 = map->mask1 = nb - 1;

#if defined(__i386__) || defined(__x86_64__)
    if (__builtin_cpu_supports("sse4.1") && __builtin_cpu_supports("popcnt")) {
	map->find = fp47m_find2_sse4;
	map->insert = fp47m_insert2_sse4;
	map->prefetch = fp47m_prefetch2_sse4;
    }
    else
#endif
    {
	map->find = fp47m_find2;
	map->insert = fp47m_insert2;
	map->prefetch = fp47m_prefetch2;
    }
    return map;
}

void fp47map_free(struct fp47map *map)
{
    if (!map)
	return;
    size_t nb = map->mask1 + (size_t) 1;
    size_t bytes = nb * map->bsize * 8;
    if (bytes >= MTHRESH) {
	int rc = munmap(map->bb, bytes);
	assert(rc == 0);
    }
    else
	free(map->bb);
    free(map);
}

struct stash {
    // Since bucket entries are looked up by index+tag, we also need to
    // remember the index (there are actually two symmetrical indices and
    // we may store either of them, or possibly the smaller one).
    uint32_t i1[4];
    union bent be[4];
};

void FASTCALL fp47m_prefetch2(uint64_t fp, const struct fp47map *map)
{
    dFP2I;
    union bent *bb = map->bb;
    __builtin_prefetch(bb + 2 * i1);
    __builtin_prefetch(bb + 2 * i2);
}

static void FASTCALL fp47m_prefetch4(uint64_t fp, const struct fp47map *map)
{
    dFP2I;
    union bent *bb = map->bb;
    __builtin_prefetch(bb + 4 * i1);
    __builtin_prefetch(bb + 4 * i2);
}

static void FASTCALL fp47m_prefetch4re(uint64_t fp, const struct fp47map *map)
{
    dFP2I; ResizeI;
    union bent *bb = map->bb;
    __builtin_prefetch(bb + 4 * i1);
    __builtin_prefetch(bb + 4 * i2);
}

static inline unsigned find(int bsize, union bent *b1, union bent *b2, uint32_t tag, uint32_t *mpos)
{
    unsigned n = 0;
    if (bsize > 0 && unlikely(b1[0].tag == tag)) mpos[n++] = b1[0].pos;
    if (bsize > 0 && unlikely(b2[0].tag == tag)) mpos[n++] = b2[0].pos;
    if (bsize > 1 && unlikely(b1[1].tag == tag)) mpos[n++] = b1[1].pos;
    if (bsize > 1 && unlikely(b2[1].tag == tag)) mpos[n++] = b2[1].pos;
    if (bsize > 2 && unlikely(b1[2].tag == tag)) mpos[n++] = b1[2].pos;
    if (bsize > 2 && unlikely(b2[2].tag == tag)) mpos[n++] = b2[2].pos;
    if (bsize > 3 && unlikely(b1[3].tag == tag)) mpos[n++] = b1[3].pos;
    if (bsize > 3 && unlikely(b2[3].tag == tag)) mpos[n++] = b2[3].pos;
    return n;
}

#define FindSt1(j)						\
    do {							\
	const struct stash *st = (const void *) map->stash;	\
	if (likely(st->be[j].tag != tag))			\
	    break;						\
	if (unlikely(st->i1[j] != i1))				\
	    break;						\
	*mpos = st->be[j].pos;					\
	n += 1;							\
    } while (0)

unsigned FASTCALL fp47m_find2(uint64_t fp, const struct fp47map *map, uint32_t *mpos)
{
    dFP2I;
    union bent *bb = map->bb;
    return find(2, bb + 2 * i1, bb + 2 * i2, tag, mpos);
}

static unsigned FASTCALL fp47m_find2st1(uint64_t fp, const struct fp47map *map, uint32_t *mpos)
{
    dFP2I;
    union bent *bb = map->bb;
    unsigned n = find(2, bb + 2 * i1, bb + 2 * i2, tag, mpos);
    i1 = (i1 < i2) ? i1 : i2;
    FindSt1(0);
    return n;
}

static unsigned FASTCALL fp47m_find2st4(uint64_t fp, const struct fp47map *map, uint32_t *mpos)
{
    dFP2I;
    union bent *bb = map->bb;
    unsigned n = find(2, bb + 2 * i1, bb + 2 * i2, tag, mpos);
    i1 = (i1 < i2) ? i1 : i2;
    FindSt1(0); FindSt1(1); FindSt1(2); FindSt1(3);
    return n;
}

static unsigned FASTCALL fp47m_find4(uint64_t fp, const struct fp47map *map, uint32_t *mpos)
{
    dFP2I;
    union bent *bb = map->bb;
    return find(4, bb + 4 * i1, bb + 4 * i2, tag, mpos);
}

static unsigned FASTCALL fp47m_find4st1(uint64_t fp, const struct fp47map *map, uint32_t *mpos)
{
    dFP2I;
    union bent *bb = map->bb;
    unsigned n = find(4, bb + 4 * i1, bb + 4 * i2, tag, mpos);
    i1 = (i1 < i2) ? i1 : i2;
    FindSt1(0);
    return n;
}

static unsigned FASTCALL fp47m_find4st4(uint64_t fp, const struct fp47map *map, uint32_t *mpos)
{
    dFP2I;
    union bent *bb = map->bb;
    unsigned n = find(4, bb + 4 * i1, bb + 4 * i2, tag, mpos);
    i1 = (i1 < i2) ? i1 : i2;
    FindSt1(0); FindSt1(1); FindSt1(2); FindSt1(3);
    return n;
}

static unsigned FASTCALL fp47m_find4re(uint64_t fp, const struct fp47map *map, uint32_t *mpos)
{
    dFP2I; ResizeI;
    union bent *bb = map->bb;
    return find(4, bb + 4 * i1, bb + 4 * i2, tag, mpos);
}

static unsigned FASTCALL fp47m_find4st1re(uint64_t fp, const struct fp47map *map, uint32_t *mpos)
{
    dFP2I; ResizeI;
    union bent *bb = map->bb;
    unsigned n = find(4, bb + 4 * i1, bb + 4 * i2, tag, mpos);
    FindSt1(0);
    return n;
}

static unsigned FASTCALL fp47m_find4st4re(uint64_t fp, const struct fp47map *map, uint32_t *mpos)
{
    dFP2I; ResizeI;
    union bent *bb = map->bb;
    unsigned n = find(4, bb + 4 * i1, bb + 4 * i2, tag, mpos);
    FindSt1(0); FindSt1(1); FindSt1(2); FindSt1(3);
    return n;
}

static inline bool insert(int bsize, union bent *b1, union bent *b2, union bent kbe)
{
    if (bsize > 0 && b1[0].tag == 0) return b1[0] = kbe, true;
    if (bsize > 0 && b2[0].tag == 0) return b2[0] = kbe, true;
    if (bsize > 1 && b1[1].tag == 0) return b1[1] = kbe, true;
    if (bsize > 1 && b2[1].tag == 0) return b2[1] = kbe, true;
    if (bsize > 2 && b1[2].tag == 0) return b1[2] = kbe, true;
    if (bsize > 2 && b2[2].tag == 0) return b2[2] = kbe, true;
    if (bsize > 3 && b1[3].tag == 0) return b1[3] = kbe, true;
    if (bsize > 3 && b2[3].tag == 0) return b2[3] = kbe, true;
    return false;
}

static inline bool kickloop(int bsize, union bent *bb, union bent *b1,
	uint32_t i1, union bent be, uint32_t *oi1, union bent *obe,
	uint32_t mask, int maxkick)
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
	b1 = bb + i1 * bsize;
	// Insert to the alternative bucket.
	if (bsize > 0 && b1[0].tag == 0) return b1[0] = *obe, true;
	if (bsize > 1 && b1[1].tag == 0) return b1[1] = *obe, true;
	if (bsize > 2 && b1[2].tag == 0) return b1[2] = *obe, true;
	if (bsize > 3 && b1[3].tag == 0) return b1[3] = *obe, true;
	be = *obe;
    } while (--maxkick >= 0);
    // Ran out of tries? obe already set.
    *oi1 = i1;
    return false;
}

static inline bool putstash(struct fp47map *map, uint32_t i1, union bent kbe,
	unsigned (FASTCALL *find_st1)(uint64_t fp, const struct fp47map *map, uint32_t *mpos),
	unsigned (FASTCALL *find_st4)(uint64_t fp, const struct fp47map *map, uint32_t *mpos))
{
    struct stash *st = (void *) &map->stash;
    if (likely(map->nstash == 0)) {
	st->i1[0] = i1;
	st->be[0] = kbe;
	st->i1[1] = st->i1[2] = st->i1[3] = 0;
	st->be[1] = st->be[2] = st->be[3] = BE0;
	map->find = find_st1;
	map->nstash = 1, map->cnt--;
	return true;
    }
    if (likely(map->nstash < 4)) {
	st->i1[map->nstash] = i1;
	st->be[map->nstash] = kbe;
	map->find = find_st4;
	map->nstash++, map->cnt--;
	return true;
    }
    return false;
}

#define A16(p) __builtin_assume_aligned(p, 16)

static inline void reinterp24(union bent *bb, size_t nb, union bent *bb4)
{
    for (size_t i = nb - 2; i; i -= 2) {
	union bent *b2 = bb  + 2 * i;
	union bent *b4 = bb4 + 4 * i;
	memcpy(A16(b4 + 0), A16(b2 + 0), 16);
	memcpy(A16(b4 + 4), A16(b2 + 2), 16);
	memset(A16(b4 + 2), 0, 16);
	memset(A16(b4 + 6), 0, 16);
    }
    uint64_t be0[2], be1[2];
    memcpy(&be0, A16(bb + 0), 16);
    memcpy(&be1, A16(bb + 2), 16);
    memcpy(A16(bb4 + 0), &be0, 16);
    memcpy(A16(bb4 + 4), &be1, 16);
    memset(A16(bb4 + 2), 0, 16);
    memset(A16(bb4 + 6), 0, 16);
}

static inline void reinterp44(union bent *bb, size_t nb, union bent *bb4,
	uint32_t mask0, uint32_t mask1, int logsize0)
{
    union bent *bb8 = bb4 + 4 * nb;
    for (size_t i = 0; i < nb; i++) {
	union bent b[4];
	memcpy(b, A16(bb + 4 * i), 32);
	memset(A16(bb4 + 4 * i), 0, 32);
	memset(A16(bb8 + 4 * i), 0, 32);
	unsigned j4 = 0, j8 = 0;
	for (unsigned j = 0; j < 4; j++) {
	    uint32_t tag = b[j].tag;
	    uint32_t i1 = i;
	    uint32_t i2 = i ^ tag;
	    i1 &= mask0, i2 &= mask0;
	    i1 = (i1 < i2) ? i1 : i2;
	    i1 |= tag << logsize0;
	    i2 = i1 ^ tag;
	    i1 &= mask1, i2 &= mask1;
	    union bent *b1 = bb4 + 4 * i;
	    unsigned j1 = j4;
	    unsigned eq = (i == i1) | (i == i2);
	    b1 = eq ? b1 : bb8 + 4 * i;
	    j1 = eq ? j1 : j8;
	    j4 = eq ? j4 + 1 : j4;
	    j8 = eq ? j8 : j8 + 1;
	    b1[j1] = b[j];
	}
    }
}

static int FASTCALL fp47m_insert4(uint64_t fp, struct fp47map *map, uint32_t pos);
static int FASTCALL fp47m_insert4re(uint64_t fp, struct fp47map *map, uint32_t pos);

struct re5 {
    uint32_t i1[5];
    union bent be[5];
};

// Reinsert the stashed entries and the pending entry.
static inline bool restash(struct fp47map *map, uint32_t i1, union bent kbe, bool re)
{
    struct re5 re5, ore;
    struct stash *st = (void *) &map->stash;
    unsigned n = map->nstash;
    for (unsigned j = 0; j < n; j++)
	re5.be[j] = st->be[j], re5.i1[j] = st->i1[j];
    re5.be[n] = kbe, re5.i1[n] = i1;
    memset(&ore.i1[1], 0, 16);
    memset(&ore.be[1], 0, 32);
    union bent *bb = map->bb;
    unsigned oj = 0;
    for (unsigned j = 0; j <= n; j++) {
	i1 = re5.i1[j], kbe = re5.be[j];
	uint32_t i2;
	if (re) {
	    i1 |= kbe.tag << map->logsize0;
	    i2 = (i1 ^ kbe.tag) & map->mask1;
	    i1 &= map->mask1;
	}
	else
	    i2 = (i1 ^ kbe.tag) & map->mask0;
	union bent *b1 = bb + 4 * i1;
	if (insert(4, b1, bb + 4 * i2, kbe))
	    continue;
	unsigned mask = re ? map->mask1 : map->mask0;
	int logsize = re ? map->logsize1 : map->logsize0;
	if (kickloop(4, bb, b1, i1, kbe, &i1, &kbe, mask, 2 * logsize))
	    continue;
	i2 = (i1 ^ kbe.tag) & map->mask0;
	if (re) {
	    i1 &= map->mask0;
	    i1 = (i1 < i2) ? i1 : i2;
	    i1 |= kbe.tag << map->logsize0;
	    i1 &= map->mask1;
	}
	else
	    i1 = (i1 < i2) ? i1 : i2;
	ore.i1[oj] = i1, ore.be[oj++] = kbe;
    }
    map->cnt += (size_t) n - oj;
    map->nstash = oj;
    if (unlikely(oj)) {
	memcpy(A16(st->i1), ore.i1, 16);
	memcpy(A16(st->be), ore.be, 32);
	map->find = likely(oj == 1) ?
	    (re ? fp47m_find4st1re : fp47m_find4st1) :
	    (re ? fp47m_find4st4re : fp47m_find4st4) ;
	if (unlikely(oj > 4)) {
	    map->nstash = 4;
	    return false;
	}
    }
    return true;
}

static inline int resize2(struct fp47map *map, uint32_t i1, union bent kbe)
{
    if (sizeof(size_t) < 5 && map->logsize0 == 27)
	return -2;
    size_t nb = map->mask0 + (size_t) 1;
    void *bb = allocX2(&map->bb, nb * 16);
    if (!bb)
	return -2;
    reinterp24(map->bb, nb, bb);
    if (map->bb != bb)
	free(map->bb), map->bb = bb;
    map->bsize = 4;
    map->find = fp47m_find4;
    map->insert = fp47m_insert4;
    map->prefetch = fp47m_prefetch4;
    if (restash(map, i1, kbe, false))
	return 2;
    return -1;
}

static int fp47m_resize4(struct fp47map *map, uint32_t i1, union bent kbe)
{
    if (map->logsize1 == ((sizeof(size_t) < 5) ? 26 : 32))
	return -2;
    size_t nb = map->mask1 + (size_t) 1;
    void *bb = allocX2(&map->bb, nb * 32);
    if (!bb)
	return -2;
    map->mask1 = map->mask1 << 1 | 1;
    map->logsize1++;
    reinterp44(map->bb, nb, bb, map->mask0, map->mask1, map->logsize0);
    if (map->bb != bb)
	free(map->bb), map->bb = bb;
    map->find = fp47m_find4re;
    map->insert = fp47m_insert4re;
    map->prefetch = fp47m_prefetch4re;
    if (restash(map, i1, kbe, true))
	return 2;
    return -1;
}

int FASTCALL fp47m_insert2(uint64_t fp, struct fp47map *map, uint32_t pos)
{
    dFP2I;
    union bent *bb = map->bb;
    union bent *b1 = bb + 2 * i1;
    union bent kbe = { .tag = tag, .pos = pos };
    map->cnt++;
    if (insert(2, b1, bb + 2 * i2, kbe))
	return 1;
    if (1) { // check the fill factor
	if (kickloop(2, bb, b1, i1, kbe, &i1, &kbe, map->mask0, 2 * map->logsize0))
	    return 1;
	i2 = (i1 ^ kbe.tag) & map->mask0;
	i1 = (i1 < i2) ? i1 : i2;
	if (putstash(map, i1, kbe, fp47m_find2st1, fp47m_find2st4))
	    return 1;
    }
    else
	i1 = (i1 < i2) ? i1 : i2;
    return resize2(map, i1, kbe);
}

static int FASTCALL fp47m_insert4(uint64_t fp, struct fp47map *map, uint32_t pos)
{
    dFP2I;
    union bent *bb = map->bb;
    union bent *b1 = bb + 4 * i1;
    union bent kbe = { .tag = tag, .pos = pos };
    map->cnt++;
    if (insert(4, b1, bb + 4 * i2, kbe))
	return 1;
    if (1) { // check the fill factor
	if (kickloop(4, bb, b1, i1, kbe, &i1, &kbe, map->mask0, 2 * map->logsize0))
	    return 1;
	i2 = (i1 ^ kbe.tag) & map->mask0;
	i1 = (i1 < i2) ? i1 : i2;
	if (putstash(map, i1, kbe, fp47m_find4st1, fp47m_find4st4))
	    return 1;
    }
    else
	i1 = (i1 < i2) ? i1 : i2;
    return fp47m_resize4(map, i1, kbe);
}

static int FASTCALL fp47m_insert4re(uint64_t fp, struct fp47map *map, uint32_t pos)
{
    dFP2I; ResizeI;
    union bent *bb = map->bb;
    union bent *b1 = bb + 4 * i1;
    union bent kbe = { .tag = tag, .pos = pos };
    map->cnt++;
    if (insert(4, b1, bb + 4 * i2, kbe))
	return 1;
    if (1) { // check the fill factor
	if (kickloop(4, bb, b1, i1, kbe, &i1, &kbe, map->mask1, 2 * map->logsize1))
	    return 1;
	i2 = (i1 ^ kbe.tag) & map->mask0;
	i1 &= map->mask0;
	i1 = (i1 < i2) ? i1 : i2;
	i1 |= kbe.tag << map->logsize0;
	i1 &= map->mask1;
	if (putstash(map, i1, kbe, fp47m_find4st1re, fp47m_find4st4re))
	    return 1;
    }
    return fp47m_resize4(map, i1, kbe);
}
