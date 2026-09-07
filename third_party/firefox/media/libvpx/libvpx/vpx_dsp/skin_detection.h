/*
 *  Copyright (c) 2017 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(VPX_VPX_DSP_SKIN_DETECTION_H_)
#define VPX_VPX_DSP_SKIN_DETECTION_H_

#if defined(__cplusplus)
extern "C" {
#endif

int vpx_skin_pixel(const int y, const int cb, const int cr, int motion);

#if defined(__cplusplus)
}  
#endif

#endif
