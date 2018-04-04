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

#include "fpmap.h"

// Reduce a 64-bit fingerprint to [1,UINT32_MAX].
static inline uint32_t reduce32(uint64_t fp)
{
#ifdef __SIZEOF_INT128__
    // A fast alternative to the modulo reduction
    // https://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/
    return 1 + ((UINT32_MAX * (__uint128_t) fp) >> 64);
#endif
    // Can the compiler generate efficient code?
    if (sizeof(long) > 4)
	return 1 + fp % UINT32_MAX;
    // On i686, gcc generates the __umoddi3 call instead.
    // The following is equivalent to 1 + fp % UINT32_MAX.
    fp = (fp >> 32) + (uint32_t) fp + 1;
    return fp + (fp >> 32) + ((fp + 1) >> 33);
}
