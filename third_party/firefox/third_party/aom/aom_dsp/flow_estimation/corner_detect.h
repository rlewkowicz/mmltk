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

#if !defined(AOM_AOM_DSP_FLOW_ESTIMATION_CORNER_DETECT_H_)
#define AOM_AOM_DSP_FLOW_ESTIMATION_CORNER_DETECT_H_

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <memory.h>

#include "aom_dsp/pyramid.h"
#include "aom_util/aom_pthread.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define MAX_CORNERS 4096

typedef struct corner_list {
#if CONFIG_MULTITHREAD
  pthread_mutex_t mutex;
#endif
  bool valid;
  int num_corners;
  int corners[2 * MAX_CORNERS];
} CornerList;

size_t av1_get_corner_list_size(void);

CornerList *av1_alloc_corner_list(void);

bool av1_compute_corner_list(const YV12_BUFFER_CONFIG *frame, int bit_depth,
                             int downsample_level, CornerList *corners);

#if !defined(NDEBUG)
bool aom_is_corner_list_valid(CornerList *corners);
#endif

void av1_invalidate_corner_list(CornerList *corners);

void av1_free_corner_list(CornerList *corners);

#if defined(__cplusplus)
}
#endif

#endif
