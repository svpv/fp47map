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

// A "fingerprint map" a low-level bucket manager that serves as the basis for
// hash tables.  Internally it manages "bucket entries" of this kind:
//
//	struct bent {
//	    uint32_t tag; // fingerprint tag
//	    uint32_t pos; // position
//	};
//
// These entries associate fingerprints with positions.  A "position" is
// 32-bit user data (typically an array index), and can be of any value
// (including 0 and UINT32_MAX).  To insert entries / look up positions,
// the caller supplies fingerprints.  A "fingerprint" is a 64-bit hash value
// with good statistical properties.  It further gets split into two pieces:
// the index to locate the bucket, and the "fingerptint tag" to recheck the
// entries in the bucket.  The tag is calculated in such a way that it is
// non-zero, while zeros mark empty slots.  (The actual scheme is a bit more
// complicated: we check two buckets, and the tag is also responsible for
// locating the second bucket.  This scheme is known as the cuckoo filter.)
// Thus the data structure is conceptually similar to multimap<hash,pos>:
// it leaves it up to the caller to compare the keys for exact equality.

#pragma once
#ifndef __cplusplus
#include <stddef.h>
#include <stdint.h>
#else
#include <cstddef>
#include <cstdint>
extern "C" {
#endif

// Beta version, static linking only.
#ifdef __GNUC__
#pragma GCC visibility push(hidden)
#endif

// Create a map.  The logsize parameter specifies the expected number of
// entries in the map (e.g. logsize = 10 for 1024).  There is a fairly small
// but not completely negligible chance of failure to build the map (this is
// true for any hash-based implementation which limits its worst-case
// behavior.)  The failure rate depends on the initial logsize value:
// the bigger the table, the smaller the chance that it breaks.  Therefore,
// logsize should not be too small, and had better be a realistic minimum.
struct fp47map *fp47map_new(int logsize);
void fp47map_free(struct fp47map *map);

// Since the buckets are fixed-size, the map guarantees O(1) worst-case lookup.
// Use FP47MAP_MAXFIND to specify the array size for fp47map_find().
#define FP47MAP_MAXFIND 12

#if defined(__i386__) && !defined(_WIN32) && !defined(__CYGWIN__)
#define FP47M_FASTCALL __attribute__((regparm(3)))
#else
#define FP47M_FASTCALL
#endif

// Expose the structure, to inline vfunc calls.
struct fp47map {
    // To reduce the failure rate, one or two bucket entries can be stashed.
    // There are some details which we do not disclose in this header file.
    // This guy goes first and gets the best alignment (for SIMD loads).
    unsigned char stash[48];
    // Virtual functions, depend on the bucket size, switched on resize.
    // Pass fp arg first, eax:edx may hold hash() return value.
    unsigned (FP47M_FASTCALL *find)(uint64_t fp, const struct fp47map *map, uint32_t *mpos);
    int (FP47M_FASTCALL *insert)(uint64_t fp, struct fp47map *map, uint32_t pos);
    void (FP47M_FASTCALL *prefetch)(uint64_t fp, const struct fp47map *map);
    // The buckets (malloc'd); each bucket has bsize entries.
    void *bb;
    // The total number of entries added to buckets,
    // not including the stashed entries.
    size_t cnt;
    // The number of entries in each bucket: 2, 3, or 4.
    uint8_t bsize;
    // The number of stashed entries: 0, 1, or 2.
    uint8_t nstash;
    // The number of buckets, initial and current, the logarithm: 4..32.
    uint8_t logsize0, logsize1;
    // The corresponding masks, help indexing into the buckets.
    uint32_t mask0, mask1;
    // Max iterations in the kick loop.
    uint8_t maxkick;
};

// Obtain the set of positions matching a fingerprint.
// Returns the number of matches found (up to FP47MAP_MAXFIND, typically 0 or 1).
static inline unsigned fp47map_find(const struct fp47map *map, uint64_t fp,
	uint32_t mpos[FP47MAP_MAXFIND])
{
    return map->find(fp, map, mpos);
}

// Insert a new entry, that is, a new position associated with a fingerprint.
static inline int fp47map_insert(struct fp47map *map, uint64_t fp, uint32_t pos)
{
    return map->insert(fp, map, pos);
}

// Prefetch the buckets related to a fingerprint.
static inline void fp47map_prefetch(const struct fp47map *map, uint64_t fp)
{
    map->prefetch(fp, map);
}

#ifdef __GNUC__
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif
