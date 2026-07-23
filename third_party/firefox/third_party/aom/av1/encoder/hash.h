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

#if !defined(AOM_AV1_ENCODER_HASH_H_)
#define AOM_AV1_ENCODER_HASH_H_

#include "aom/aom_integer.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct _CRC32C {
  uint32_t table[8][256];
} CRC32C;

void av1_crc32c_calculator_init(CRC32C *p_crc32c);

#define AOM_BUFFER_SIZE_FOR_BLOCK_HASH (4096)

#if defined(__cplusplus)
}  
#endif

#endif
