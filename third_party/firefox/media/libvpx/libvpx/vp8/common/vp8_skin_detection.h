/*
 *  Copyright (c) 2015 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(VPX_VP8_COMMON_VP8_SKIN_DETECTION_H_)
#define VPX_VP8_COMMON_VP8_SKIN_DETECTION_H_

#include "vp8/encoder/onyx_int.h"
#include "vpx/vpx_integer.h"
#include "vpx_dsp/skin_detection.h"
#include "vpx_scale/yv12config.h"

#if defined(__cplusplus)
extern "C" {
#endif

struct VP8_COMP;

typedef enum {
  SKIN_8X8,
  SKIN_16X16
} SKIN_DETECTION_BLOCK_SIZE;

int vp8_compute_skin_block(const uint8_t *y, const uint8_t *u, const uint8_t *v,
                           int stride, int strideuv,
                           SKIN_DETECTION_BLOCK_SIZE bsize, int consec_zeromv,
                           int curr_motion_magn);

#if defined(OUTPUT_YUV_SKINMAP)
void vp8_compute_skin_map(struct VP8_COMP *const cpi, FILE *yuv_skinmap_file);
#endif

#if defined(__cplusplus)
}  
#endif

#endif
