/*
 * Copyright 2013 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkLumaColorFilter_DEFINED)
#define SkLumaColorFilter_DEFINED

#include "include/core/SkRefCnt.h"
#include "include/core/SkTypes.h"

class SkColorFilter;

struct SK_API SkLumaColorFilter {
    static sk_sp<SkColorFilter> Make();
};

#endif
