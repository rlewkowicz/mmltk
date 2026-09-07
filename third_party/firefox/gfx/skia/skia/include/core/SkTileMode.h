/*
 * Copyright 2019 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkTileModes_DEFINED)
#define SkTileModes_DEFINED

#include "include/core/SkTypes.h"

enum class SkTileMode {
    kClamp,

    kRepeat,

    kMirror,

    kDecal,

    kLastTileMode = kDecal,
};

static constexpr int kSkTileModeCount = static_cast<int>(SkTileMode::kLastTileMode) + 1;

#endif
