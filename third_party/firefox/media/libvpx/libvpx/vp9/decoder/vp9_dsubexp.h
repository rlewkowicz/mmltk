/*
 *  Copyright (c) 2013 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(VPX_VP9_DECODER_VP9_DSUBEXP_H_)
#define VPX_VP9_DECODER_VP9_DSUBEXP_H_

#include "vpx_dsp/bitreader.h"

#if defined(__cplusplus)
extern "C" {
#endif

void vp9_diff_update_prob(vpx_reader *r, vpx_prob *p);

#if defined(__cplusplus)
}  
#endif

#endif
