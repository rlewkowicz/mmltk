/*
 * Copyright 2006 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkNoncopyable_DEFINED)
#define SkNoncopyable_DEFINED

#include "include/private/base/SkAPI.h"

class SK_API SkNoncopyable {
public:
    SkNoncopyable() = default;

    SkNoncopyable(SkNoncopyable&&) = default;
    SkNoncopyable& operator =(SkNoncopyable&&) = default;

private:
    SkNoncopyable(const SkNoncopyable&) = delete;
    SkNoncopyable& operator=(const SkNoncopyable&) = delete;
};

#endif
