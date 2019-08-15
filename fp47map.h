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
// hash tables.  It manages "bucket entries" of the two flavors:
//
//	#define FPMAP_FPTAG_BITS 16
//	struct fpmap_bent {
//	    ...                   // user data
//	    const uint16_t fptag; // fingerprint tag
//	};
//
//	#define FPMAP_FPTAG_BITS 32
//	struct fpmap_bent {
//	    ...                   // user data
//	    const uint32_t fptag; // fingerprint tag
//	};
//
// To look up the entries, the caller supplies fingerprints.  A "fingerprint" is
// a 64-bit hash value with good statistical properties.  It further gets digested
// into two pieces: the index to locate the bucket, and the "fingerptint tag"
// to recheck the entries in the bucket.  (The actual scheme is a bit more
// complicated: we check two buckets, and the fingerprint is also responsible
// for locating the second bucket.  This scheme is known as the cuckoo filter.)
// Thus the data structure is conceptually similar to multimap<hash,data>:
// it leaves it up to the caller to compare the keys for exact equality.
//
// Along with struct fpmap_bent and FPMAP_FPTAG_BITS, the caller must define
// FPMAP_BENT_SIZE, which is a decimal literal equal to sizeof(struct fpmap_bent).
// The literals are used to give distinct names to ABI functions, so that it is
// possible to use a few different maps in the same program.  (Inline functions
// are not renamed though, so currently it isn't possible to use two different
// maps in a single source file.)

#ifndef __cplusplus
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#else
#include <cstddef>
#include <cstdint>
extern "C" {
#endif

// Beta version, static linking only.
#ifdef __GNUC__
#pragma GCC visibility push(hidden)
#endif

#define FPMAP_NAME3_(h, v, name) fp##h##map##v##_##name
#define FPMAP_NAME3(h, v, name) FPMAP_NAME3_(h, v, name)

#if FPMAP_FPTAG_BITS == 16
#define FPMAP_NAME(name) FPMAP_NAME3(31, FPMAP_BENT_SIZE, name)
#elif FPMAP_FPTAG_BITS == 32
#define FPMAP_NAME(name) FPMAP_NAME3(47, FPMAP_BENT_SIZE, name)
#else
#error "invalid FPMAP_FPTAG_BITS"
#endif

static_assert(sizeof(struct fpmap_bent) == FPMAP_BENT_SIZE, "");
static_assert(offsetof(struct fpmap_bent, fptag) == FPMAP_BENT_SIZE - FPMAP_FPTAG_BITS / 8,
	"fptag the last member of struct fpmap_bent");

#define fpmap_new FPMAP_NAME(new)
#define fpmap_free FPMAP_NAME(free)

// Create a map.  The logsize parameter specifies the expected number of
// entries in the map (e.g. logsize = 10 for 1024).  There is a fairly small
// but not completely negligible chance of failure to build the map (this is
// true for any hash-based implementation which limits its worst-case
// behavior.)  The failure rate depends on the initial logsize value:
// the bigger the table, the smaller the chance that it breaks.  Therefore,
// logsize should not be too small, and had better be a realistic lower bound.
struct fpmap *fpmap_new(int logsize);
void fpmap_free(struct fpmap *map);

// Since the buckets are fixed-size, the map guarantees O(1) worst-case lookup.
// Use FPMAP_MAXFIND to specify the array size for fpmap_find().
#define FPMAP_MAXFIND 10
#ifdef __cplusplus
#define FPMAP_pMAXFIND 10 // for use in function prototypes
#else
#define FPMAP_pMAXFIND static 10
#endif

// i386 convention: on Windows, stick to fastcall, for compatibility with msvc.
#if (defined(_WIN32) || defined(__CYGWIN__)) && \
    (defined(_M_IX86) || defined(__i386__))
#define FPMAP_MSFASTCALL 1
#if defined(__GNUC__)
#define FPMAP_FASTCALL __attribute__((fastcall))
#else
#define FPMAP_FASTCALL __fastcall
#endif
#else // otherwise, use regparm(3).
#define FPMAP_MSFASTCALL 0
#if defined(__i386__)
#define FPMAP_FASTCALL __attribute__((regparm(3)))
#else
#define FPMAP_FASTCALL
#endif
#endif

// fastcall has trouble passing uint64_t in registers.
#if FPMAP_MSFASTCALL
#define FPMAP_pFP64 uint32_t lo, uint32_t hi
#define FPMAP_aFP64(fp) fp, fp >> 32
#else
#define FPMAP_pFP64 uint64_t fp
#define FPMAP_aFP64(fp) fp
#endif

// Check if CPU registers are 64-bit.
#if (SIZE_MAX > UINT32_MAX) || defined(__x86_64__)
#define FPMAP_REG64 1
#else
#define FPMAP_REG64 0
#endif

// To reduce the failure rate, one or two bucket entries can be stashed.
struct fpmap_stash {
    struct fpmap_bent be[2];
    // Since bucket entries are looked up by index+fptag, we also need to
    // remember the index (there are actually two symmetrical indices and
    // we store the smaller one).  Furthermore, on 64-bit platforms we can
    // check index+fptag as a single var.
#if FPMAP_REG64
    uint64_t lo[2];
#else
    uint32_t lo[2];
#endif
};

// Expose the structure, to inline vfunc calls.
struct fpmap {
    // This guy goes first and gets the best alignment (the same as buckets).
    struct fpmap_stash stash;
    // Virtual functions, depend on the bucket size, switched on resize.
    // Pass fp arg first, eax:edx may hold hash() return value.
    size_t (FPMAP_FASTCALL *find)(FPMAP_pFP64, const struct fpmap *set,
	    struct fpmap_bent *match[FPMAP_pMAXFIND]);
    struct fpmap_bent *(FPMAP_FASTCALL *insert)(FPMAP_pFP64, struct fpmap *map);
    // The buckets (malloc'd); each bucket has bsize entries.
    // Two-dimensional structure is emulated with pointer arithmetic.
    struct fpmap_bent *bb;
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
};

// Obtain the set of entries matching the fingerprint.
// Returns the number of matches found (up to FPMAP_MAXFIND, typically 0 or 1).
static inline size_t fpmap_find(const struct fpmap *map, uint64_t fp,
	struct fpmap_bent *match[FPMAP_pMAXFIND])
{
    return map->find(FPMAP_aFP64(fp), map, match);
}

// Request a new entry associated with the fingerprint.
static inline struct fpmap_bent *fpmap_insert(struct fpmap *map, uint64_t fp)
{
    return map->insert(FPMAP_aFP64(fp), map);
}

#ifdef __GNUC__
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif
