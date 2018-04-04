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

// A "fingerprint map" is a data structure useful for building big string
// tables (up to a few million strings, up to 4 gigabytes total).  For each
// string, the caller computes its 64-bit fingerprint, and the data structure
// provides a 32-bit slot (also called a position).  There is a possibility
// of a birthday collision (different strings map to the same slot), which
// the caller must check.  There is also a possibility of insertion failure,
// in which case fpmap_get returns NULL.  In both cases, the caller should
// simply rebuild the map from scratch, hashing the strings with a different
// hash seed.

#pragma once
#include <stdint.h>

struct fpmap *fpmap_new(int logsize);
void fpmap_free(struct fpmap *map);

// Get the position on a 64-bit fingerprint.
uint32_t *fpmap_get(struct fpmap *map, uint64_t fp);
