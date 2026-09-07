/*
 *  Copyright (c) 2019 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(VPX_VPX_UTIL_VPX_TIMESTAMP_H_)
#define VPX_VPX_UTIL_VPX_TIMESTAMP_H_

#include <assert.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct vpx_rational64 {
  int64_t num;       
  int den;           
} vpx_rational64_t;  

static INLINE int gcd(int64_t a, int b) {
  int r;  
  assert(a >= 0);
  assert(b > 0);
  while (b != 0) {
    r = (int)(a % b);
    a = b;
    b = r;
  }

  return (int)a;
}

static INLINE void reduce_ratio(vpx_rational64_t *ratio) {
  const int denom = gcd(ratio->num, ratio->den);
  ratio->num /= denom;
  ratio->den /= denom;
}

#if defined(__cplusplus)
}  
#endif

#endif
