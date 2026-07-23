/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved.
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#if !defined(AOM_AOM_DSP_FLOW_ESTIMATION_DISFLOW_H_)
#define AOM_AOM_DSP_FLOW_ESTIMATION_DISFLOW_H_

#include <stdbool.h>

#include "aom_dsp/flow_estimation/flow_estimation.h"
#include "aom_scale/yv12config.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define DISFLOW_PYRAMID_LEVELS 12

#define DISFLOW_PATCH_SIZE_LOG2 3
#define DISFLOW_PATCH_SIZE (1 << DISFLOW_PATCH_SIZE_LOG2)
#define DISFLOW_PATCH_CENTER ((DISFLOW_PATCH_SIZE / 2) - 1)

#define DISFLOW_DERIV_SCALE_LOG2 3
#define DISFLOW_DERIV_SCALE (1 << DISFLOW_DERIV_SCALE_LOG2)

#define DISFLOW_STEP_SIZE 1.0

#define DISFLOW_STEP_SIZE_THRESOLD (1. / 8.)

#define DISFLOW_MAX_ITR 4

#define DISFLOW_INTERP_BITS 14

typedef struct {
  double *buf0;

  double *u;
  double *v;

  int width;
  int height;
  int stride;
} FlowField;

bool av1_compute_global_motion_disflow(
    TransformationType type, YV12_BUFFER_CONFIG *src, YV12_BUFFER_CONFIG *ref,
    int bit_depth, int downsample_level, MotionModel *motion_models,
    int num_motion_models, bool *mem_alloc_failed);

#if defined(__cplusplus)
}
#endif

#endif
