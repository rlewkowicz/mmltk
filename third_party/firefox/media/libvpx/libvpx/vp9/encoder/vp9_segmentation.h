/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(VPX_VP9_ENCODER_VP9_SEGMENTATION_H_)
#define VPX_VP9_ENCODER_VP9_SEGMENTATION_H_

#include "vp9/common/vp9_blockd.h"
#include "vp9/encoder/vp9_encoder.h"

#if defined(__cplusplus)
extern "C" {
#endif

void vp9_enable_segmentation(struct segmentation *seg);
void vp9_disable_segmentation(struct segmentation *seg);

void vp9_disable_segfeature(struct segmentation *seg, int segment_id,
                            SEG_LVL_FEATURES feature_id);
void vp9_clear_segdata(struct segmentation *seg, int segment_id,
                       SEG_LVL_FEATURES feature_id);

void vp9_psnr_aq_mode_setup(struct segmentation *seg);

void vp9_perceptual_aq_mode_setup(struct VP9_COMP *cpi,
                                  struct segmentation *seg);

void vp9_set_segment_data(struct segmentation *seg, signed char *feature_data,
                          unsigned char abs_delta);

void vp9_choose_segmap_coding_method(VP9_COMMON *cm, MACROBLOCKD *xd);

void vp9_reset_segment_features(struct segmentation *seg);

#if defined(__cplusplus)
}  
#endif

#endif
