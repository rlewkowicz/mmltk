/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(VPX_VP8_COMMON_SWAPYV12BUFFER_H_)
#define VPX_VP8_COMMON_SWAPYV12BUFFER_H_

#include "vpx_scale/yv12config.h"

#if defined(__cplusplus)
extern "C" {
#endif

void vp8_swap_yv12_buffer(YV12_BUFFER_CONFIG *new_frame,
                          YV12_BUFFER_CONFIG *last_frame);

#if defined(__cplusplus)
}  
#endif

#endif
