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

#if !defined(AOM_AOM_PORTS_AOM_TIMER_H_)
#define AOM_AOM_PORTS_AOM_TIMER_H_

#include "config/aom_config.h"

#if CONFIG_OS_SUPPORT

#include <stddef.h>
#include <stdint.h>

#include <sys/time.h>

#if !defined(timersub)
#define timersub(a, b, result)                       \
  do {                                               \
    (result)->tv_sec = (a)->tv_sec - (b)->tv_sec;    \
    (result)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
    if ((result)->tv_usec < 0) {                     \
      --(result)->tv_sec;                            \
      (result)->tv_usec += 1000000;                  \
    }                                                \
  } while (0)
#endif

struct aom_usec_timer {
  struct timeval begin, end;
};

static inline void aom_usec_timer_start(struct aom_usec_timer *t) {
  gettimeofday(&t->begin, NULL);
}

static inline void aom_usec_timer_mark(struct aom_usec_timer *t) {
  gettimeofday(&t->end, NULL);
}

static inline int64_t aom_usec_timer_elapsed(struct aom_usec_timer *t) {
  struct timeval diff;

  timersub(&t->end, &t->begin, &diff);
  return ((int64_t)diff.tv_sec) * 1000000 + diff.tv_usec;
}

#else

#if !defined(timersub)
#define timersub(a, b, result)
#endif

struct aom_usec_timer {
  void *dummy;
};

static inline void aom_usec_timer_start(struct aom_usec_timer *t) { (void)t; }

static inline void aom_usec_timer_mark(struct aom_usec_timer *t) { (void)t; }

static inline int aom_usec_timer_elapsed(struct aom_usec_timer *t) {
  (void)t;
  return 0;
}

#endif

#endif
