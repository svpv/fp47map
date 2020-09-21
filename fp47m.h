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

#include "fp47map.h"

#define likely(cond) __builtin_expect(!!(cond), 1)
#define unlikely(cond) __builtin_expect(cond, 0)

// The inline functions rely heavily on constant propagation.
#define inline inline __attribute__((always_inline))

union bent {
    uint64_t u64;
    struct {
	uint32_t fptag;
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

// Fingerprint -> indexes + fptag.
// Note that the two buckets are completely symmetrical with regard to xor,
// i.e. the information about "the first and true" index is not preserved.
// This looses about 1 bit out of 32+logsize bits of hashing material.
#define dFP2I					\
    uint32_t i1 = fp >> 32;			\
    uint32_t fptag = mod32(fp);			\
    uint32_t i2 = i1 ^ fptag;			\
    i1 &= map->mask0;				\
    i2 &= map->mask0

#pragma GCC visibility push(hidden)

#if defined(__i386__) || defined(__x86_64__)
unsigned FP47M_FASTCALL fp47m_find2_sse4(uint64_t fp, const struct fp47map *map, uint32_t *mpos);
int FP47M_FASTCALL fp47m_insert2_sse4(uint64_t fp, struct fp47map *map, uint32_t pos);
#endif

#pragma GCC visibility pop
