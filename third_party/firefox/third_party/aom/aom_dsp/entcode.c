/*
 * Copyright (c) 2001-2016, Alliance for Open Media. All rights reserved.
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include "aom_dsp/entcode.h"

uint32_t od_ec_tell_frac(uint32_t nbits_total, uint32_t rng) {
  uint32_t nbits;
  int l;
  int i;
  nbits = nbits_total << OD_BITRES;
  l = 0;
  for (i = OD_BITRES; i-- > 0;) {
    int b;
    rng = rng * rng >> 15;
    b = (int)(rng >> 16);
    l = l << 1 | b;
    rng >>= b;
  }
  return nbits - l;
}
