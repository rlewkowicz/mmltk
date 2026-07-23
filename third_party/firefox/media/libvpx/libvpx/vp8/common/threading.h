/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(VPX_VP8_COMMON_THREADING_H_)
#define VPX_VP8_COMMON_THREADING_H_

#include "./vpx_config.h"

#if defined(__cplusplus)
extern "C" {
#endif

#if CONFIG_OS_SUPPORT && CONFIG_MULTITHREAD

#include <semaphore.h>


#include <errno.h>
#include <unistd.h>
#include <sched.h>
#define vp8_sem_t sem_t
#define vp8_sem_init sem_init
static INLINE int vp8_sem_wait(vp8_sem_t *sem) {
  int ret;
  while ((ret = sem_wait(sem)) == -1 && errno == EINTR) {
  }
  return ret;
}
#define vp8_sem_post sem_post
#define vp8_sem_destroy sem_destroy

#if defined(__unix__) || 0
#define thread_sleep(nms)
#else
#define thread_sleep(nms) sched_yield();
#endif


#if VPX_ARCH_X86 || VPX_ARCH_X86_64
#include "vpx_ports/x86.h"
#else
#define x86_pause_hint()
#endif

#include "vpx_util/vpx_atomics.h"

static INLINE void vp8_atomic_spin_wait(
    int mb_col, const vpx_atomic_int *last_row_current_mb_col,
    const int nsync) {
  while (mb_col > (vpx_atomic_load_acquire(last_row_current_mb_col) - nsync)) {
    x86_pause_hint();
    thread_sleep(0);
  }
}

#endif

#if defined(__cplusplus)
}  
#endif

#endif
