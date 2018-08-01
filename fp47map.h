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

// A "fingerprint map" is an auxiliary data structure for building big string
// tables (many million strings).  The structure maps a fingerprint to the set
// of candidate positions.  A "fingerprint" is a full 64-bit hash value with
// good statistical properties.  A "position" is a non-zero 32-bit value which
// is basically an offset into the string table.  Put simply, the structure
// maps hash->string; but because 64 bits are not enough to identify strings,
// the caller should recheck the candidates, typically by calling strcmp(3).
//
// There is a fairly small but not completely negligible chance of failure
// to build the map (insertion can fail, leaving the structure in a somewhat
// inconsistent state).  In this case, the caller should rebuild the map
// from scratch, hashing the strings with a different hash seed.

#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

// There are two components that contribute to the probability of failure:
// 1) cycles in the cuckoo graph (cannot arrange the items into buckets);
// 2) heavy birthday collisions (more than two items end up with the same hash).
// Both components depend on the logsize parameter, which is the logarithm of
// the expected size of the map (e.g. 20 for 1 million strings).  Internally,
// each fingerprint is digested into a reduced 32-bit hash value and the
// logsize-bit index at which the value is stored, so effectively only
// 32+logsize bits out of 64 bits are used.  Although the structure does have
// a (somewhat limited) capability for resizing, the logsize should not be
// too small, and had better be a realistic lower bound.
struct fpmap *fpmap_new(int logsize);
void fpmap_free(struct fpmap *map);

// Exposes only a part of the structure, just enough to inline the calls.
struct fpmap {
    size_t (*find)(void *map, uint64_t fp, uint32_t pos[10]);
    bool (*insert)(void *map, uint64_t fp, uint32_t pos);
};

// Get the candidate positions on a 64-bit fingerprint.
// Returns the number of positions found (at most 10, typically 0 or 1).
static inline size_t fpmap_find(struct fpmap *map, uint64_t fp, uint32_t pos[10])
{
    return map->find(map, fp, pos);
}

// Compile with -DFPMAP_DEBUG during the development stage.
#ifdef FPMAP_DEBUG
#include <assert.h>
#endif

// Insert a position on a fingerprint.  Dups are not detected (thus fpmap_insert
// should be called only if fpmap_find failed to locate the matching string).
// Returns true on success.  Returns false on insertion failure, which means
// that a series of evictions failed, and an unrelated slot has been kicked out.
// Unless false negatives are permitted, the only option is to rebuild the map
// from scratch, hashing the strings with a different hash seed.
static inline bool fpmap_insert(struct fpmap *map, uint64_t fp, uint32_t pos)
{
    // Recall that positions are non-negative.
    // Zero is used internally to mark free slots.
#ifdef FPMAP_DEBUG
    assert(pos > 0);
#endif
    return map->insert(map, fp, pos);
}

#ifdef __cplusplus
}
#endif
