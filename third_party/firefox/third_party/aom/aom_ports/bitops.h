/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved.
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#if !defined(AOM_AOM_PORTS_BITOPS_H_)
#define AOM_AOM_PORTS_BITOPS_H_

#include <assert.h>
#include <stdint.h>

#include "config/aom_config.h"

#if defined(_MSC_VER)
#if defined(_M_X64) || defined(_M_IX86) || defined(_M_ARM64) || defined(_M_ARM)
#include <intrin.h>
#define USE_MSC_INTRINSICS
#endif
#endif

#if defined(__cplusplus)
extern "C" {
#endif



#if defined(__GNUC__) && \
    ((__GNUC__ == 3 && __GNUC_MINOR__ >= 4) || __GNUC__ >= 4)
static inline int get_msb(unsigned int n) {
  assert(n != 0);
  return 31 ^ __builtin_clz(n);
}
#elif defined(USE_MSC_INTRINSICS)
#pragma intrinsic(_BitScanReverse)

static inline int get_msb(unsigned int n) {
  unsigned long first_set_bit;
  assert(n != 0);
  _BitScanReverse(&first_set_bit, n);
  return first_set_bit;
}
#else
static inline int get_msb(unsigned int n) {
  int log = 0;
  unsigned int value = n;

  assert(n != 0);

  for (int shift = 16; shift != 0; shift >>= 1) {
    const unsigned int x = value >> shift;
    if (x != 0) {
      value = x;
      log += shift;
    }
  }
  return log;
}
#endif

static inline int aom_ceil_log2(int n) {
  if (n < 2) return 0;
  return get_msb(n - 1) + 1;
}

#if defined(__GNUC__) && \
    ((__GNUC__ == 3 && __GNUC_MINOR__ >= 4) || __GNUC__ >= 4)
static inline int aom_clzll(uint64_t n) { return __builtin_clzll(n); }
#elif defined(USE_MSC_INTRINSICS)
#if defined(_M_X64) || defined(_M_ARM64)
#pragma intrinsic(_BitScanReverse64)
#endif

static inline int aom_clzll(uint64_t n) {
  assert(n != 0);
  unsigned long first_set_bit;  // NOLINT(runtime/int)
#if defined(_M_X64) || defined(_M_ARM64)
  const unsigned char bit_set =
      _BitScanReverse64(&first_set_bit, (unsigned __int64)n);
#else
  const unsigned long n_hi = (unsigned long)(n >> 32);  // NOLINT(runtime/int)
  if (n_hi != 0) {
    const unsigned char bit_set = _BitScanReverse(&first_set_bit, n_hi);
    assert(bit_set != 0);
    (void)bit_set;
    return 31 ^ (int)first_set_bit;
  }
  const unsigned char bit_set =
      _BitScanReverse(&first_set_bit, (unsigned long)n);  // NOLINT(runtime/int)
#endif
  assert(bit_set != 0);
  (void)bit_set;
  return 63 ^ (int)first_set_bit;
}
#undef USE_MSC_INTRINSICS
#else
static inline int aom_clzll(uint64_t n) {
  assert(n != 0);

  int res = 0;
  uint64_t high_bit = 1ULL << 63;
  while (!(n & high_bit)) {
    res++;
    n <<= 1;
  }
  return res;
}
#endif

#if defined(__cplusplus)
}  
#endif

#endif
