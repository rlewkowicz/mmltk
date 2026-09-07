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

#if !defined(AOM_AV1_COMMON_AV1_INV_TXFM1D_CFG_H_)
#define AOM_AV1_COMMON_AV1_INV_TXFM1D_CFG_H_
#include "av1/common/av1_inv_txfm1d.h"

static const int8_t inv_start_range[TX_SIZES_ALL] = {
  5,  
  6,  
  7,  
  7,  
  7,  
  5,  
  5,  
  6,  
  6,  
  6,  
  6,  
  6,  
  6,  
  6,  
  6,  
  7,  
  7,  
  7,  
  7,  
};

extern const int8_t *av1_inv_txfm_shift_ls[TX_SIZES_ALL];

#define INV_COS_BIT 12

#endif
