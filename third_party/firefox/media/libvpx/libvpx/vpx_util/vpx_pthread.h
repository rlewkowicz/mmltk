// Copyright 2024 Google Inc. All Rights Reserved.
// Use of this source code is governed by a BSD-style license

#if !defined(VPX_VPX_UTIL_VPX_PTHREAD_H_)
#define VPX_VPX_UTIL_VPX_PTHREAD_H_

#include "./vpx_config.h"

#if CONFIG_MULTITHREAD

#if defined(__cplusplus)
extern "C" {
#endif

#include <pthread.h>  // NOLINT
#define THREADFN void *
#define THREAD_EXIT_SUCCESS NULL

#if defined(__cplusplus)
}  
#endif

#endif

#endif
