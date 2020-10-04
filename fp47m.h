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

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#include "fp47map.h"

#define likely(cond) __builtin_expect(!!(cond), 1)
#define unlikely(cond) __builtin_expect(cond, 0)

#define FASTCALL FP47M_FASTCALL

// The inline functions rely heavily on constant propagation.
#define inline inline __attribute__((always_inline))
#define NOINLINE __attribute__((noinline))

union bent {
    uint64_t u64;
    struct {
	uint32_t tag;
	uint32_t pos;
    };
};

#define BE0 (union bent){0}

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

// Fingerprint -> indexes + tag.
// Note that the two buckets are completely symmetrical with regard to xor,
// i.e. the information about "the first and true" index is not preserved.
// This looses about 1 bit out of 32+logsize bits of hashing material.
#define dFP2I					\
    uint32_t i1 = fp >> 32;			\
    uint32_t tag = mod32(fp);			\
    uint32_t i2 = i1 ^ tag;			\
    i1 &= map->mask0;				\
    i2 &= map->mask0

// When the table is resized, indexes need extra high bits.
#define ResizeI					\
    do {					\
	i1 = (i2 < i1) ? i2 : i1;		\
	i1 |= tag << map->logsize0;		\
	i2 = i1 ^ tag;				\
	i1 &= map->mask1;			\
	i2 &= map->mask1;			\
    } while (0)

#pragma GCC visibility push(hidden)

// The initial set of virtual functions.
unsigned FASTCALL fp47m_find2(uint64_t fp, const struct fp47map *map, uint32_t *mpos);
int FASTCALL fp47m_insert2(uint64_t fp, struct fp47map *map, uint32_t pos);
void FASTCALL fp47m_prefetch2(uint64_t fp, const struct fp47map *map);

#if defined(__i386__) || defined(__x86_64__)
unsigned FASTCALL fp47m_find2_sse4(uint64_t fp, const struct fp47map *map, uint32_t *mpos);
int FASTCALL fp47m_insert2_sse4(uint64_t fp, struct fp47map *map, uint32_t pos);
void FASTCALL fp47m_prefetch2_sse4(uint64_t fp, const struct fp47map *map);
#endif

#pragma GCC visibility pop

// malloc/mmap threshold
#define MTHRESH 99999

static inline void *allocX2(void **pp, size_t bytes)
{
    void *p;
    if (bytes >= MTHRESH) {
	p = mremap(*pp, bytes, 2 * bytes, MREMAP_MAYMOVE);
	if (p == MAP_FAILED)
	    return NULL;
	*pp = p;
    }
    else if (2 * bytes >= MTHRESH) {
	p = mmap(NULL, 2 * bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (p == MAP_FAILED)
	    return NULL;
    }
    else
	p = aligned_alloc(32, 2 * bytes);
    return p;
}
