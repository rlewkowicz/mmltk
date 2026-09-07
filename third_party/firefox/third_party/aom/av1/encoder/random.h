/*
 * Copyright (c) 2017, Alliance for Open Media. All rights reserved.
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#if !defined(AOM_AV1_ENCODER_RANDOM_H_)
#define AOM_AV1_ENCODER_RANDOM_H_

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

static inline uint32_t lcg_next(uint32_t *state) {
  *state = (uint32_t)(*state * 1103515245ULL + 12345);
  return *state;
}

static inline uint32_t lcg_rand16(uint32_t *state) {
  return (lcg_next(state) / 65536) % 32768;
}

static inline uint32_t lcg_randint(uint32_t *state, uint32_t n) {
  uint64_t v = ((uint64_t)lcg_next(state) * n) >> 32;
  return (uint32_t)v;
}

static inline uint32_t lcg_randrange(uint32_t *state, uint32_t lo,
                                     uint32_t hi) {
  assert(lo < hi);
  return lo + lcg_randint(state, hi - lo);
}

static inline void lcg_pick(int n, int k, int *out, unsigned int *seed) {
  assert(0 <= k && k <= n);
  for (int i = 0; i < k; i++) {
    int v;

  resample:
    v = (int)lcg_randint(seed, n);
    for (int j = 0; j < i; j++) {
      if (v == out[j]) {
        goto resample;
      }
    }

    out[i] = v;
  }
}

#if defined(__cplusplus)
}  
#endif

#endif
