/*
 * Copyright 2021 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkBlenders_DEFINED)
#define SkBlenders_DEFINED

#include "include/core/SkBlender.h"

class SK_API SkBlenders {
public:
    static sk_sp<SkBlender> Arithmetic(float k1, float k2, float k3, float k4, bool enforcePremul);

private:
    SkBlenders() = delete;
};

#endif
