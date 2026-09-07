// Copyright 2011 Google Inc. All Rights Reserved.
// Use of this source code is governed by a BSD-style license

#if !defined(WEBP_UTILS_FILTERS_UTILS_H_)
#define WEBP_UTILS_FILTERS_UTILS_H_

#include "src/dsp/dsp.h"
#include "src/webp/types.h"

#if defined(__cplusplus)
extern "C" {
#endif

WEBP_FILTER_TYPE WebPEstimateBestFilter(const uint8_t* data,
                                        int width, int height, int stride);

#if defined(__cplusplus)
}    
#endif

#endif
