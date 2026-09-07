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

#if !defined(AOM_AV1_COMMON_FRAME_BUFFERS_H_)
#define AOM_AV1_COMMON_FRAME_BUFFERS_H_

#include "aom/aom_frame_buffer.h"
#include "aom/aom_integer.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct InternalFrameBuffer {
  uint8_t *data;
  size_t size;
  int in_use;
} InternalFrameBuffer;

typedef struct InternalFrameBufferList {
  int num_internal_frame_buffers;
  InternalFrameBuffer *int_fb;
} InternalFrameBufferList;

int av1_alloc_internal_frame_buffers(InternalFrameBufferList *list);

void av1_free_internal_frame_buffers(InternalFrameBufferList *list);

void av1_zero_unused_internal_frame_buffers(InternalFrameBufferList *list);

int av1_get_frame_buffer(void *cb_priv, size_t min_size,
                         aom_codec_frame_buffer_t *fb);

int av1_release_frame_buffer(void *cb_priv, aom_codec_frame_buffer_t *fb);

#if defined(__cplusplus)
}  
#endif

#endif
