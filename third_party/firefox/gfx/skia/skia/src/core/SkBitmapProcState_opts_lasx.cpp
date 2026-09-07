/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/private/base/SkFeatures.h" // IWYU pragma: keep
#include "src/core/SkOptsTargets.h" // IWYU pragma: keep

#if defined(SK_CPU_LOONGARCH) && !defined(SK_ENABLE_OPTIMIZE_SIZE)


#define SK_OPTS_TARGET SK_OPTS_TARGET_LASX
#include "src/opts/SkOpts_SetTarget.h"

#include "src/core/SkBitmapProcState.h"
#include "src/opts/SkBitmapProcState_opts.h"

#include "src/opts/SkOpts_RestoreTarget.h"

namespace SkOpts {
    void Init_BitmapProcState_lasx() {
        S32_alpha_D32_filter_DX = lasx::S32_alpha_D32_filter_DX;
    }
}  

#endif
