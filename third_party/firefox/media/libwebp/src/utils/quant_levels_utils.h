// Copyright 2011 Google Inc. All Rights Reserved.
// Use of this source code is governed by a BSD-style license

#if !defined(WEBP_UTILS_QUANT_LEVELS_UTILS_H_)
#define WEBP_UTILS_QUANT_LEVELS_UTILS_H_

#include <stdlib.h>

#include "src/webp/types.h"

#if defined(__cplusplus)
extern "C" {
#endif

int QuantizeLevels(uint8_t* const data, int width, int height, int num_levels,
                   uint64_t* const sse);

#if defined(__cplusplus)
}    
#endif

#endif
