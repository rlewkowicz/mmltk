/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkLogPriority_DEFINED)
#define SkLogPriority_DEFINED


enum class SkLogPriority : int {
    kFatal = 0,
    kError = 1,
    kWarning = 2,
    kInfo = 3,
    kDebug = 4,
};

#endif
