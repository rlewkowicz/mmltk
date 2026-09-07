// Copyright 2022 Google Inc. All Rights Reserved.
// Use of this source code is governed by a BSD-style license
#if !defined(WEBP_SHARPYUV_SHARPYUV_CPU_H_)
#define WEBP_SHARPYUV_SHARPYUV_CPU_H_

#include "sharpyuv/sharpyuv.h"

#undef WEBP_EXTERN
#define WEBP_EXTERN extern
#define VP8GetCPUInfo SharpYuvGetCPUInfo
#include "src/dsp/cpu.h"

#endif
