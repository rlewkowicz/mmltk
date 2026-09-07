/*
 * Copyright (c) 2024, Alliance for Open Media. All rights reserved.
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#if !defined(AOM_AOM_UTIL_AOM_PTHREAD_H_)
#define AOM_AOM_UTIL_AOM_PTHREAD_H_

#include "config/aom_config.h"

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
