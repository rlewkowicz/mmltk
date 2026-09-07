/*
 *  Copyright (c) 2014 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(VPX_VP9_ENCODER_VP9_RESIZE_H_)
#define VPX_VP9_ENCODER_VP9_RESIZE_H_

#include <stdio.h>
#include "vpx/vpx_integer.h"

#if defined(__cplusplus)
extern "C" {
#endif

void vp9_resize_plane(const uint8_t *const input, int height, int width,
                      int in_stride, uint8_t *output, int height2, int width2,
                      int out_stride);

#if CONFIG_VP9_HIGHBITDEPTH
void vp9_highbd_resize_plane(const uint8_t *const input, int height, int width,
                             int in_stride, uint8_t *output, int height2,
                             int width2, int out_stride, int bd);
#endif

#if defined(__cplusplus)
}  
#endif

#endif
