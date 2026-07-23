// Copyright 2022 Google Inc. All Rights Reserved.
// Use of this source code is governed by a BSD-style license

#if !defined(WEBP_SHARPYUV_SHARPYUV_CSP_H_)
#define WEBP_SHARPYUV_SHARPYUV_CSP_H_

#include "sharpyuv/sharpyuv.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef enum {
  kSharpYuvRangeFull,     
  kSharpYuvRangeLimited   
} SharpYuvRange;

typedef struct {
  float kr;
  float kb;
  int bit_depth;  
  SharpYuvRange range;
} SharpYuvColorSpace;

SHARPYUV_EXTERN void SharpYuvComputeConversionMatrix(
    const SharpYuvColorSpace* yuv_color_space,
    SharpYuvConversionMatrix* matrix);

typedef enum {
  kSharpYuvMatrixWebp = 0,
  kSharpYuvMatrixRec601Limited,
  kSharpYuvMatrixRec601Full,
  kSharpYuvMatrixRec709Limited,
  kSharpYuvMatrixRec709Full,
  kSharpYuvMatrixNum
} SharpYuvMatrixType;

SHARPYUV_EXTERN const SharpYuvConversionMatrix* SharpYuvGetConversionMatrix(
    SharpYuvMatrixType matrix_type);

#if defined(__cplusplus)
}  
#endif

#endif
