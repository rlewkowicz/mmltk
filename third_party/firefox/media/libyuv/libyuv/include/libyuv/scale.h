/*
 *  Copyright 2011 The LibYuv Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS. All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(INCLUDE_LIBYUV_SCALE_H_)
#define INCLUDE_LIBYUV_SCALE_H_

#include "libyuv/basic_types.h"

#if defined(__cplusplus)
namespace libyuv {
extern "C" {
#endif

typedef enum FilterMode {
  kFilterNone = 0,      
  kFilterLinear = 1,    
  kFilterBilinear = 2,  
  kFilterBox = 3        
} FilterModeEnum;

LIBYUV_API
int ScalePlane(const uint8_t* src,
               int src_stride,
               int src_width,
               int src_height,
               uint8_t* dst,
               int dst_stride,
               int dst_width,
               int dst_height,
               enum FilterMode filtering);

LIBYUV_API
int ScalePlane_16(const uint16_t* src,
                  int src_stride,
                  int src_width,
                  int src_height,
                  uint16_t* dst,
                  int dst_stride,
                  int dst_width,
                  int dst_height,
                  enum FilterMode filtering);

LIBYUV_API
int ScalePlane_12(const uint16_t* src,
                  int src_stride,
                  int src_width,
                  int src_height,
                  uint16_t* dst,
                  int dst_stride,
                  int dst_width,
                  int dst_height,
                  enum FilterMode filtering);


LIBYUV_API
int I420Scale(const uint8_t* src_y,
              int src_stride_y,
              const uint8_t* src_u,
              int src_stride_u,
              const uint8_t* src_v,
              int src_stride_v,
              int src_width,
              int src_height,
              uint8_t* dst_y,
              int dst_stride_y,
              uint8_t* dst_u,
              int dst_stride_u,
              uint8_t* dst_v,
              int dst_stride_v,
              int dst_width,
              int dst_height,
              enum FilterMode filtering);

LIBYUV_API
int I420Scale_16(const uint16_t* src_y,
                 int src_stride_y,
                 const uint16_t* src_u,
                 int src_stride_u,
                 const uint16_t* src_v,
                 int src_stride_v,
                 int src_width,
                 int src_height,
                 uint16_t* dst_y,
                 int dst_stride_y,
                 uint16_t* dst_u,
                 int dst_stride_u,
                 uint16_t* dst_v,
                 int dst_stride_v,
                 int dst_width,
                 int dst_height,
                 enum FilterMode filtering);

LIBYUV_API
int I420Scale_12(const uint16_t* src_y,
                 int src_stride_y,
                 const uint16_t* src_u,
                 int src_stride_u,
                 const uint16_t* src_v,
                 int src_stride_v,
                 int src_width,
                 int src_height,
                 uint16_t* dst_y,
                 int dst_stride_y,
                 uint16_t* dst_u,
                 int dst_stride_u,
                 uint16_t* dst_v,
                 int dst_stride_v,
                 int dst_width,
                 int dst_height,
                 enum FilterMode filtering);


LIBYUV_API
int I444Scale(const uint8_t* src_y,
              int src_stride_y,
              const uint8_t* src_u,
              int src_stride_u,
              const uint8_t* src_v,
              int src_stride_v,
              int src_width,
              int src_height,
              uint8_t* dst_y,
              int dst_stride_y,
              uint8_t* dst_u,
              int dst_stride_u,
              uint8_t* dst_v,
              int dst_stride_v,
              int dst_width,
              int dst_height,
              enum FilterMode filtering);

LIBYUV_API
int I444Scale_16(const uint16_t* src_y,
                 int src_stride_y,
                 const uint16_t* src_u,
                 int src_stride_u,
                 const uint16_t* src_v,
                 int src_stride_v,
                 int src_width,
                 int src_height,
                 uint16_t* dst_y,
                 int dst_stride_y,
                 uint16_t* dst_u,
                 int dst_stride_u,
                 uint16_t* dst_v,
                 int dst_stride_v,
                 int dst_width,
                 int dst_height,
                 enum FilterMode filtering);

LIBYUV_API
int I444Scale_12(const uint16_t* src_y,
                 int src_stride_y,
                 const uint16_t* src_u,
                 int src_stride_u,
                 const uint16_t* src_v,
                 int src_stride_v,
                 int src_width,
                 int src_height,
                 uint16_t* dst_y,
                 int dst_stride_y,
                 uint16_t* dst_u,
                 int dst_stride_u,
                 uint16_t* dst_v,
                 int dst_stride_v,
                 int dst_width,
                 int dst_height,
                 enum FilterMode filtering);

LIBYUV_API
int I422Scale(const uint8_t* src_y,
              int src_stride_y,
              const uint8_t* src_u,
              int src_stride_u,
              const uint8_t* src_v,
              int src_stride_v,
              int src_width,
              int src_height,
              uint8_t* dst_y,
              int dst_stride_y,
              uint8_t* dst_u,
              int dst_stride_u,
              uint8_t* dst_v,
              int dst_stride_v,
              int dst_width,
              int dst_height,
              enum FilterMode filtering);

LIBYUV_API
int I422Scale_16(const uint16_t* src_y,
                 int src_stride_y,
                 const uint16_t* src_u,
                 int src_stride_u,
                 const uint16_t* src_v,
                 int src_stride_v,
                 int src_width,
                 int src_height,
                 uint16_t* dst_y,
                 int dst_stride_y,
                 uint16_t* dst_u,
                 int dst_stride_u,
                 uint16_t* dst_v,
                 int dst_stride_v,
                 int dst_width,
                 int dst_height,
                 enum FilterMode filtering);

LIBYUV_API
int I422Scale_12(const uint16_t* src_y,
                 int src_stride_y,
                 const uint16_t* src_u,
                 int src_stride_u,
                 const uint16_t* src_v,
                 int src_stride_v,
                 int src_width,
                 int src_height,
                 uint16_t* dst_y,
                 int dst_stride_y,
                 uint16_t* dst_u,
                 int dst_stride_u,
                 uint16_t* dst_v,
                 int dst_stride_v,
                 int dst_width,
                 int dst_height,
                 enum FilterMode filtering);


LIBYUV_API
int NV12Scale(const uint8_t* src_y,
              int src_stride_y,
              const uint8_t* src_uv,
              int src_stride_uv,
              int src_width,
              int src_height,
              uint8_t* dst_y,
              int dst_stride_y,
              uint8_t* dst_uv,
              int dst_stride_uv,
              int dst_width,
              int dst_height,
              enum FilterMode filtering);

LIBYUV_API
int NV24Scale(const uint8_t* src_y,
              int src_stride_y,
              const uint8_t* src_uv,
              int src_stride_uv,
              int src_width,
              int src_height,
              uint8_t* dst_y,
              int dst_stride_y,
              uint8_t* dst_uv,
              int dst_stride_uv,
              int dst_width,
              int dst_height,
              enum FilterMode filtering);

#if defined(__cplusplus)
LIBYUV_API
int Scale(const uint8_t* src_y,
          const uint8_t* src_u,
          const uint8_t* src_v,
          int src_stride_y,
          int src_stride_u,
          int src_stride_v,
          int src_width,
          int src_height,
          uint8_t* dst_y,
          uint8_t* dst_u,
          uint8_t* dst_v,
          int dst_stride_y,
          int dst_stride_u,
          int dst_stride_v,
          int dst_width,
          int dst_height,
          LIBYUV_BOOL interpolate);

LIBYUV_API
void SetUseReferenceImpl(LIBYUV_BOOL use);
#endif

#if defined(__cplusplus)
}  
}  
#endif

#endif
