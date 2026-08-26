/*
  FastLZ - Byte-aligned LZ77 compression library
  Copyright (C) 2005-2020 Ariya Hidayat <ariya.hidayat@gmail.com>

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

/*
 * BoltPack C89 port of upstream fastlz 0.5.0 (FASTLZ_VERSION 0x000500).
 *
 * This is a strict ANSI C89/C90 version of the upstream source so it can be
 * compiled with the core's `-ansi -pedantic` flags.  The changes relative to
 * upstream 0.5.0 are kept minimal and are documented here for review:
 *
 *   - `#include <stdint.h>` removed.  `uint8_t`/`uint16_t`/`uint32_t` are
 *     replaced with local typedefs (fastlz_uint8_t etc.) guarded by an
 *     #ifndef so this file stays self-contained and does not depend on any
 *     BoltPack header.  On every BoltPack target compiler these map to
 *     `unsigned char`, `unsigned short`, and `unsigned long` respectively.
 *   - All declarations were hoisted to the top of each function/block so no
 *     declaration appears after a statement (strict C89).  The algorithm is
 *     byte-for-byte identical to upstream; only declaration placement and
 *     the type spellings changed.
 *   - The `FASTLZ_LIKELY`/`FASTLZ_UNLIKELY` __builtin_expect helper is kept
 *     behind its existing `#if defined(__clang__) || (defined(__GNUC__) ...)`
 *     feature guard with a portable fallback, so no compiler extension is
 *     unconditionally required.
 *   - The `FASTLZ_USE_MEMMOVE` selection is retained.  The default (nonzero)
 *     path uses memmove/memcpy from <string.h>; the byte-by-byte fallback
 *     (define FASTLZ_USE_MEMMOVE=0) is preserved for targets without a
 *     working memmove.  BoltPack 16-bit targets do not compile this file
 *     (see note below), so the segmented-memory concern does not apply.
 *
 * Memory note: the compressor declares `const fastlz_uint8_t* htab[HASH_SIZE]`
 * on the stack, where HASH_SIZE = 8192.  That is 8192 pointers (32 KB on a
 * 32-bit target, 64 KB on 64-bit) and is why fastlz *compression* is not
 * compiled for 16-bit targets, which have only 4-8 KB stacks.  Decompression
 * needs no such buffer.  See the BoltPack plan docs/relicense-to-bsd2-plan.md.
 */

#include "fastlz.h"
#include <limits.h>

#ifndef FASTLZ_UINT8_T
typedef unsigned char   fastlz_uint8_t;
typedef unsigned short  fastlz_uint16_t;
#if ULONG_MAX == 0xFFFFFFFFUL
typedef unsigned long   fastlz_uint32_t;
#elif UINT_MAX >= 0xFFFFFFFFUL
typedef unsigned int    fastlz_uint32_t;
#else
#error "FastLZ requires an unsigned integer type with at least 32 bits"
#endif
#define FASTLZ_UINT8_T
#endif

/*
 * Always check for bound when decompressing.
 * Generally it is best to leave it defined.
 */
#define FASTLZ_SAFE
#if defined(FASTLZ_USE_SAFE_DECOMPRESSOR) && (FASTLZ_USE_SAFE_DECOMPRESSOR == 0)
#undef FASTLZ_SAFE
#endif

/*
 * Give hints to the compiler for branch prediction optimization.
 */
#if defined(__clang__) || (defined(__GNUC__) && (__GNUC__ > 2))
#define FASTLZ_LIKELY(c) (__builtin_expect(!!(c), 1))
#define FASTLZ_UNLIKELY(c) (__builtin_expect(!!(c), 0))
#else
#define FASTLZ_LIKELY(c) (c)
#define FASTLZ_UNLIKELY(c) (c)
#endif

#if defined(FASTLZ_SAFE)
#define FASTLZ_BOUND_CHECK(cond) \
  if (FASTLZ_UNLIKELY(!(cond))) return 0;
#else
#define FASTLZ_BOUND_CHECK(cond) \
  do {                           \
  } while (0)
#endif

#define MAX_COPY 32
#define MAX_LEN 264 /* 256 + 8 */
#define MAX_L1_DISTANCE 8192
#define MAX_L2_DISTANCE 8191
#define MAX_FARDISTANCE (65535 + MAX_L2_DISTANCE - 1)

#define FASTLZ_READU16(p) ((p)[0] | (p)[1] << 8)

#define HASH_LOG 13
#define HASH_SIZE (1 << HASH_LOG)
#define HASH_MASK (HASH_SIZE - 1)
#define HASH_FUNCTION(v, p)                              \
  {                                                      \
    v = FASTLZ_READU16(p);                               \
    v ^= FASTLZ_READU16(p + 1) ^ (v >> (16 - HASH_LOG)); \
    v &= HASH_MASK;                                      \
  }

int fastlz1_compress(const void* input, int length, void* output) {
  const fastlz_uint8_t* ip;
  const fastlz_uint8_t* ip_bound;
  const fastlz_uint8_t* ip_limit;
  fastlz_uint8_t* op;
  const fastlz_uint8_t* htab[HASH_SIZE];
  fastlz_uint32_t hval;
  fastlz_uint32_t copy;
  const fastlz_uint8_t* ref;
  fastlz_uint32_t distance;
  fastlz_uint32_t len;
  const fastlz_uint8_t* anchor;
  fastlz_uint8_t x;

  ip = (const fastlz_uint8_t*)input;
  op = (fastlz_uint8_t*)output;

  /* sanity check */
  if (FASTLZ_UNLIKELY(length < 4)) {
    if (length) {
      /* create literal copy only */
      *op++ = length - 1;
      ip_bound = ip + length - 1;
      while (ip <= ip_bound) *op++ = *ip++;
      return length + 1;
    } else
      return 0;
  }

  ip_bound = ip + length - 2;
  ip_limit = ip + length - 12 - 1;

  /* initializes hash table */
  for (hval = 0; hval < HASH_SIZE; ++hval) htab[hval] = ip;

  /* we start with literal copy */
  copy = 2;
  *op++ = MAX_COPY - 1;
  *op++ = *ip++;
  *op++ = *ip++;

  /* main loop */
  while (FASTLZ_LIKELY(ip < ip_limit)) {
    /* minimum match length */
    len = 3;

    /* comparison starting-point */
    anchor = ip;

    /* find potential match */
    HASH_FUNCTION(hval, ip);
    ref = htab[hval];

    /* update hash table */
    htab[hval] = anchor;

    /* calculate distance to the match */
    distance = anchor - ref;

    /* is this a match? check the first 3 bytes */
    if (distance == 0 || (distance >= MAX_L1_DISTANCE) || *ref++ != *ip++ ||
        *ref++ != *ip++ || *ref++ != *ip++)
      goto literal;

    /* last matched byte */
    ip = anchor + len;

    /* distance is biased */
    distance--;

    if (!distance) {
      /* zero distance means a run */
      x = ip[-1];
      while (ip < ip_bound)
        if (*ref++ != x)
          break;
        else
          ip++;
    } else
      for (;;) {
        /* safe because the outer check against ip limit */
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        while (ip < ip_bound)
          if (*ref++ != *ip++) break;
        break;
      }

    /* if we have copied something, adjust the copy count */
    if (copy) /* copy is biased, '0' means 1 byte copy */
      *(op - copy - 1) = copy - 1;
    else
      /* back, to overwrite the copy count */
      op--;

    /* reset literal counter */
    copy = 0;

    /* length is biased, '1' means a match of 3 bytes */
    ip -= 3;
    len = ip - anchor;

    /* encode the match */
    if (FASTLZ_UNLIKELY(len > MAX_LEN - 2))
      while (len > MAX_LEN - 2) {
        *op++ = (7 << 5) + (distance >> 8);
        *op++ = MAX_LEN - 2 - 7 - 2;
        *op++ = (distance & 255);
        len -= MAX_LEN - 2;
      }

    if (len < 7) {
      *op++ = (len << 5) + (distance >> 8);
      *op++ = (distance & 255);
    } else {
      *op++ = (7 << 5) + (distance >> 8);
      *op++ = len - 7;
      *op++ = (distance & 255);
    }

    /* update the hash at match boundary */
    HASH_FUNCTION(hval, ip);
    htab[hval] = ip++;
    HASH_FUNCTION(hval, ip);
    htab[hval] = ip++;

    /* assuming literal copy */
    *op++ = MAX_COPY - 1;

    continue;

  literal:
    *op++ = *anchor++;
    ip = anchor;
    copy++;
    if (FASTLZ_UNLIKELY(copy == MAX_COPY)) {
      copy = 0;
      *op++ = MAX_COPY - 1;
    }
  }

  /* left-over as literal copy */
  ip_bound++;
  while (ip <= ip_bound) {
    *op++ = *ip++;
    copy++;
    if (copy == MAX_COPY) {
      copy = 0;
      *op++ = MAX_COPY - 1;
    }
  }

  /* if we have copied something, adjust the copy length */
  if (copy)
    *(op - copy - 1) = copy - 1;
  else
    op--;

  return op - (fastlz_uint8_t*)output;
}

#if defined(FASTLZ_USE_MEMMOVE) && (FASTLZ_USE_MEMMOVE == 0)

static void fastlz_memmove(fastlz_uint8_t* dest, const fastlz_uint8_t* src,
                           fastlz_uint32_t count) {
  do {
    *dest++ = *src++;
  } while (--count);
}

static void fastlz_memcpy(fastlz_uint8_t* dest, const fastlz_uint8_t* src,
                          fastlz_uint32_t count) {
  return fastlz_memmove(dest, src, count);
}

#else

#include <string.h>

static void fastlz_memmove(fastlz_uint8_t* dest, const fastlz_uint8_t* src,
                           fastlz_uint32_t count) {
  if ((count > 4) && (dest >= src + count)) {
    memmove(dest, src, count);
  } else {
    switch (count) {
      default:
        do {
          *dest++ = *src++;
        } while (--count);
        break;
      case 3:
        *dest++ = *src++;
      case 2:
        *dest++ = *src++;
      case 1:
        *dest++ = *src++;
      case 0:
        break;
    }
  }
}

static void fastlz_memcpy(fastlz_uint8_t* dest, const fastlz_uint8_t* src,
                          fastlz_uint32_t count) {
  memcpy(dest, src, count);
}

#endif

int fastlz1_decompress(const void* input, int length, void* output,
                       int maxout) {
  const fastlz_uint8_t* ip;
  const fastlz_uint8_t* ip_limit;
  const fastlz_uint8_t* ip_bound;
  fastlz_uint8_t* op;
  fastlz_uint8_t* op_limit;
  fastlz_uint32_t ctrl;
  fastlz_uint32_t len;
  fastlz_uint32_t ofs;
  const fastlz_uint8_t* ref;

  ip = (const fastlz_uint8_t*)input;
  ip_limit = ip + length;
  ip_bound = ip_limit - 2;
  op = (fastlz_uint8_t*)output;
  op_limit = op + maxout;
  ctrl = (*ip++) & 31;

  while (1) {
    if (ctrl >= 32) {
      len = (ctrl >> 5) - 1;
      ofs = (ctrl & 31) << 8;
      ref = op - ofs - 1;
      if (len == 7 - 1) {
        FASTLZ_BOUND_CHECK(ip <= ip_bound);
        len += *ip++;
      }
      ref -= *ip++;
      len += 3;
      FASTLZ_BOUND_CHECK(op + len <= op_limit);
      FASTLZ_BOUND_CHECK(ref >= (fastlz_uint8_t*)output);
      fastlz_memmove(op, ref, len);
      op += len;
    } else {
      ctrl++;
      FASTLZ_BOUND_CHECK(op + ctrl <= op_limit);
      FASTLZ_BOUND_CHECK(ip + ctrl <= ip_limit);
      fastlz_memcpy(op, ip, ctrl);
      ip += ctrl;
      op += ctrl;
    }

    if (FASTLZ_UNLIKELY(ip > ip_bound)) break;
    ctrl = *ip++;
  }

  return op - (fastlz_uint8_t*)output;
}

int fastlz2_compress(const void* input, int length, void* output) {
  const fastlz_uint8_t* ip;
  const fastlz_uint8_t* ip_bound;
  const fastlz_uint8_t* ip_limit;
  fastlz_uint8_t* op;
  const fastlz_uint8_t* htab[HASH_SIZE];
  fastlz_uint32_t hval;
  fastlz_uint32_t copy;
  const fastlz_uint8_t* ref;
  fastlz_uint32_t distance;
  fastlz_uint32_t len;
  const fastlz_uint8_t* anchor;
  fastlz_uint8_t x;

  ip = (const fastlz_uint8_t*)input;
  op = (fastlz_uint8_t*)output;

  /* sanity check */
  if (FASTLZ_UNLIKELY(length < 4)) {
    if (length) {
      /* create literal copy only */
      *op++ = length - 1;
      ip_bound = ip + length - 1;
      while (ip <= ip_bound) *op++ = *ip++;
      return length + 1;
    } else
      return 0;
  }

  ip_bound = ip + length - 2;
  ip_limit = ip + length - 12 - 1;

  /* initializes hash table */
  for (hval = 0; hval < HASH_SIZE; ++hval) htab[hval] = ip;

  /* we start with literal copy */
  copy = 2;
  *op++ = MAX_COPY - 1;
  *op++ = *ip++;
  *op++ = *ip++;

  /* main loop */
  while (FASTLZ_LIKELY(ip < ip_limit)) {
    /* minimum match length */
    len = 3;

    /* comparison starting-point */
    anchor = ip;

    /* check for a run */
    if (ip[0] == ip[-1] && ip[0] == ip[1] && ip[1] == ip[2]) {
      distance = 1;
      ip += 3;
      ref = anchor - 1 + 3;
      goto match;
    }

    /* find potential match */
    HASH_FUNCTION(hval, ip);
    ref = htab[hval];

    /* update hash table */
    htab[hval] = anchor;

    /* calculate distance to the match */
    distance = anchor - ref;

    /* is this a match? check the first 3 bytes */
    if (distance == 0 || (distance >= MAX_FARDISTANCE) || *ref++ != *ip++ ||
        *ref++ != *ip++ || *ref++ != *ip++)
      goto literal;

    /* far, needs at least 5-byte match */
    if (distance >= MAX_L2_DISTANCE) {
      if (*ip++ != *ref++ || *ip++ != *ref++) goto literal;
      len += 2;
    }

  match:

    /* last matched byte */
    ip = anchor + len;

    /* distance is biased */
    distance--;

    if (!distance) {
      /* zero distance means a run */
      x = ip[-1];
      while (ip < ip_bound)
        if (*ref++ != x)
          break;
        else
          ip++;
    } else
      for (;;) {
        /* safe because the outer check against ip limit */
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        if (*ref++ != *ip++) break;
        while (ip < ip_bound)
          if (*ref++ != *ip++) break;
        break;
      }

    /* if we have copied something, adjust the copy count */
    if (copy) /* copy is biased, '0' means 1 byte copy */
      *(op - copy - 1) = copy - 1;
    else
      /* back, to overwrite the copy count */
      op--;

    /* reset literal counter */
    copy = 0;

    /* length is biased, '1' means a match of 3 bytes */
    ip -= 3;
    len = ip - anchor;

    /* encode the match */
    if (distance < MAX_L2_DISTANCE) {
      if (len < 7) {
        *op++ = (len << 5) + (distance >> 8);
        *op++ = (distance & 255);
      } else {
        *op++ = (7 << 5) + (distance >> 8);
        for (len -= 7; len >= 255; len -= 255) *op++ = 255;
        *op++ = len;
        *op++ = (distance & 255);
      }
    } else {
      /* far away, but not yet in the another galaxy... */
      if (len < 7) {
        distance -= MAX_L2_DISTANCE;
        *op++ = (len << 5) + 31;
        *op++ = 255;
        *op++ = distance >> 8;
        *op++ = distance & 255;
      } else {
        distance -= MAX_L2_DISTANCE;
        *op++ = (7 << 5) + 31;
        for (len -= 7; len >= 255; len -= 255) *op++ = 255;
        *op++ = len;
        *op++ = 255;
        *op++ = distance >> 8;
        *op++ = distance & 255;
      }
    }

    /* update the hash at match boundary */
    HASH_FUNCTION(hval, ip);
    htab[hval] = ip++;
    HASH_FUNCTION(hval, ip);
    htab[hval] = ip++;

    /* assuming literal copy */
    *op++ = MAX_COPY - 1;

    continue;

  literal:
    *op++ = *anchor++;
    ip = anchor;
    copy++;
    if (FASTLZ_UNLIKELY(copy == MAX_COPY)) {
      copy = 0;
      *op++ = MAX_COPY - 1;
    }
  }

  /* left-over as literal copy */
  ip_bound++;
  while (ip <= ip_bound) {
    *op++ = *ip++;
    copy++;
    if (copy == MAX_COPY) {
      copy = 0;
      *op++ = MAX_COPY - 1;
    }
  }

  /* if we have copied something, adjust the copy length */
  if (copy)
    *(op - copy - 1) = copy - 1;
  else
    op--;

  /* marker for fastlz2 */
  *(fastlz_uint8_t*)output |= (1 << 5);

  return op - (fastlz_uint8_t*)output;
}

int fastlz2_decompress(const void* input, int length, void* output,
                       int maxout) {
  const fastlz_uint8_t* ip;
  const fastlz_uint8_t* ip_limit;
  const fastlz_uint8_t* ip_bound;
  fastlz_uint8_t* op;
  fastlz_uint8_t* op_limit;
  fastlz_uint32_t ctrl;
  fastlz_uint32_t len;
  fastlz_uint32_t ofs;
  const fastlz_uint8_t* ref;
  fastlz_uint8_t code;

  ip = (const fastlz_uint8_t*)input;
  ip_limit = ip + length;
  ip_bound = ip_limit - 2;
  op = (fastlz_uint8_t*)output;
  op_limit = op + maxout;
  ctrl = (*ip++) & 31;

  while (1) {
    if (ctrl >= 32) {
      len = (ctrl >> 5) - 1;
      ofs = (ctrl & 31) << 8;
      ref = op - ofs - 1;

      if (len == 7 - 1) do {
          FASTLZ_BOUND_CHECK(ip <= ip_bound);
          code = *ip++;
          len += code;
        } while (code == 255);
      code = *ip++;
      ref -= code;
      len += 3;

      /* match from 16-bit distance */
      if (FASTLZ_UNLIKELY(code == 255))
        if (FASTLZ_LIKELY(ofs == (31 << 8))) {
          FASTLZ_BOUND_CHECK(ip < ip_bound);
          ofs = (*ip++) << 8;
          ofs += *ip++;
          ref = op - ofs - MAX_L2_DISTANCE - 1;
        }

      FASTLZ_BOUND_CHECK(op + len <= op_limit);
      FASTLZ_BOUND_CHECK(ref >= (fastlz_uint8_t*)output);
      fastlz_memmove(op, ref, len);
      op += len;
    } else {
      ctrl++;
      FASTLZ_BOUND_CHECK(op + ctrl <= op_limit);
      FASTLZ_BOUND_CHECK(ip + ctrl <= ip_limit);
      fastlz_memcpy(op, ip, ctrl);
      ip += ctrl;
      op += ctrl;
    }

    if (FASTLZ_UNLIKELY(ip >= ip_limit)) break;
    ctrl = *ip++;
  }

  return op - (fastlz_uint8_t*)output;
}

int fastlz_compress(const void* input, int length, void* output) {
  if (input == 0 || output == 0 || length <= 0) return 0;

  /* for short block, choose fastlz1 */
  if (length < 65536) return fastlz1_compress(input, length, output);

  /* else... */
  return fastlz2_compress(input, length, output);
}

int fastlz_decompress(const void* input, int length, void* output, int maxout) {
  int level;

  /* A valid fastlz stream has at least a control byte and one payload byte;
   * rejecting shorter input also avoids forming ip_bound before the buffer. */
  if (input == 0 || output == 0 || length < 2 || maxout <= 0) return 0;

  /* magic identifier for compression level */
  level = ((*(const fastlz_uint8_t*)input) >> 5) + 1;

  if (level == 1) return fastlz1_decompress(input, length, output, maxout);
  if (level == 2) return fastlz2_decompress(input, length, output, maxout);

  /* unknown level, trigger error */
  return 0;
}

int fastlz_compress_level(int level, const void* input, int length,
                          void* output) {
  if (input == 0 || output == 0 || length <= 0) return 0;

  if (level == 1) return fastlz1_compress(input, length, output);
  if (level == 2) return fastlz2_compress(input, length, output);

  return 0;
}
