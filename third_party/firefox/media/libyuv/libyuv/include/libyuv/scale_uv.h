/*
 *  Copyright 2020 The LibYuv Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS. All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(INCLUDE_LIBYUV_SCALE_UV_H_)
#define INCLUDE_LIBYUV_SCALE_UV_H_

#include "libyuv/basic_types.h"
#include "libyuv/scale.h"  // For FilterMode

#if defined(__cplusplus)
namespace libyuv {
extern "C" {
#endif

LIBYUV_API
int UVScale(const uint8_t* src_uv,
            int src_stride_uv,
            int src_width,
            int src_height,
            uint8_t* dst_uv,
            int dst_stride_uv,
            int dst_width,
            int dst_height,
            enum FilterMode filtering);

LIBYUV_API
int UVScale_16(const uint16_t* src_uv,
               int src_stride_uv,
               int src_width,
               int src_height,
               uint16_t* dst_uv,
               int dst_stride_uv,
               int dst_width,
               int dst_height,
               enum FilterMode filtering);

#if defined(__cplusplus)
}  
}  
#endif

#endif
