/*
 * Copyright 2020 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkTPin_DEFINED)
#define SkTPin_DEFINED

#include <algorithm>

template <typename T>
static constexpr const T& SkTPin(const T& x, const T& lo, const T& hi) {
    return std::max(lo, std::min(x, hi));
}

#endif
