/*
 *  Copyright (c) 2015 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(VPX_VPX_PORTS_VPX_ONCE_H_)
#define VPX_VPX_PORTS_VPX_ONCE_H_

#include "vpx_config.h"


#if CONFIG_MULTITHREAD && HAVE_PTHREAD_H
#include <pthread.h>
static void once(void (*func)(void)) {
  static pthread_once_t lock = PTHREAD_ONCE_INIT;
  pthread_once(&lock, func);
}

#else

static void once(void (*func)(void)) {
  static volatile int done;

  if (!done) {
    func();
    done = 1;
  }
}
#endif

#endif
