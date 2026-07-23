/*
 *  Copyright (c) 2014 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(VPX_VP9_ENCODER_VP9_AQ_COMPLEXITY_H_)
#define VPX_VP9_ENCODER_VP9_AQ_COMPLEXITY_H_

#if defined(__cplusplus)
extern "C" {
#endif

#include "vp9/common/vp9_enums.h"

struct VP9_COMP;
struct macroblock;

void vp9_caq_select_segment(struct VP9_COMP *cpi, struct macroblock *,
                            BLOCK_SIZE bs, int mi_row, int mi_col,
                            int projected_rate);

void vp9_setup_in_frame_q_adj(struct VP9_COMP *cpi);

#if defined(__cplusplus)
}  
#endif

#endif
