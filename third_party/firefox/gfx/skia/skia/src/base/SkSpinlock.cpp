/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/base/SkSpinlock.h"

#include "include/private/base/SkFeatures.h"
#include "include/private/base/SkThreadAnnotations.h"

    static void debug_trace() {}

#if SK_CPU_X64_LEVEL >= SK_CPU_X64_LEVEL_SSE2
    #include <emmintrin.h>
    static void do_pause() { _mm_pause(); }
#else
    static void do_pause() {  }
#endif

void SkSpinlock::contendedAcquire() {
    debug_trace();

    SK_POTENTIALLY_BLOCKING_REGION_BEGIN;
    while (fLocked.exchange(true, std::memory_order_acquire)) {
        do_pause();
    }
    SK_POTENTIALLY_BLOCKING_REGION_END;
}
