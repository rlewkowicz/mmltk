/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(VPX_VPX_PORTS_VPX_TIMER_H_)
#define VPX_VPX_PORTS_VPX_TIMER_H_

#include "./vpx_config.h"

#include "vpx/vpx_integer.h"

#if CONFIG_OS_SUPPORT

#include <time.h>

#if !defined(timersub_ns)
#define timersub_ns(a, b, result)                    \
  do {                                               \
    (result)->tv_sec = (a)->tv_sec - (b)->tv_sec;    \
    (result)->tv_nsec = (a)->tv_nsec - (b)->tv_nsec; \
    if ((result)->tv_nsec < 0) {                     \
      --(result)->tv_sec;                            \
      (result)->tv_nsec += 1000000000;               \
    }                                                \
  } while (0)
#endif

struct vpx_usec_timer {
  struct timespec begin, end;
};

static INLINE void vpx_usec_timer_start(struct vpx_usec_timer *t) {
#if defined(CLOCK_MONOTONIC_RAW)
  clock_gettime(CLOCK_MONOTONIC_RAW, &t->begin);
#else
  clock_gettime(CLOCK_MONOTONIC, &t->begin);
#endif
}

static INLINE void vpx_usec_timer_mark(struct vpx_usec_timer *t) {
#if defined(CLOCK_MONOTONIC_RAW)
  clock_gettime(CLOCK_MONOTONIC_RAW, &t->end);
#else
  clock_gettime(CLOCK_MONOTONIC, &t->end);
#endif
}

static INLINE int64_t vpx_usec_timer_elapsed(struct vpx_usec_timer *t) {
  struct timespec diff;

  timersub_ns(&t->end, &t->begin, &diff);
  return (int64_t)diff.tv_sec * 1000000 + diff.tv_nsec / 1000;
}

#else

#if !defined(timersub_ns)
#define timersub_ns(a, b, result)
#endif

struct vpx_usec_timer {
  void *dummy;
};

static INLINE void vpx_usec_timer_start(struct vpx_usec_timer *t) {}

static INLINE void vpx_usec_timer_mark(struct vpx_usec_timer *t) {}

static INLINE int vpx_usec_timer_elapsed(struct vpx_usec_timer *t) { return 0; }

#endif

#endif
