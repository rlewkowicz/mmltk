/*
 * Copyright 2025 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkDrawTypes_DEFINED)
#define SkDrawTypes_DEFINED

#include <cstddef>

enum class SkDrawCoverage : bool {
    kNo = false,
    kYes = true,
};

constexpr size_t kSkBlitterContextSize = 3332;

#endif
