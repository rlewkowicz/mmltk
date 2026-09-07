// Copyright 2013 Google Inc. All Rights Reserved.
// Use of this source code is governed by a BSD-style license

#if !defined(WEBP_UTILS_QUANT_LEVELS_DEC_UTILS_H_)
#define WEBP_UTILS_QUANT_LEVELS_DEC_UTILS_H_

#include "src/webp/types.h"

#if defined(__cplusplus)
extern "C" {
#endif

int WebPDequantizeLevels(uint8_t* const data, int width, int height, int stride,
                         int strength);

#if defined(__cplusplus)
}    
#endif

#endif
