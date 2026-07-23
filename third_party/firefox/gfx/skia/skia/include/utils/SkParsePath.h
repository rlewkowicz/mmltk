
/*
 * Copyright 2006 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */


#if !defined(SkParsePath_DEFINED)
#define SkParsePath_DEFINED

#include "include/core/SkPath.h"

class SkString;

class SK_API SkParsePath {
public:
    static std::optional<SkPath> FromSVGString(const char str[]);
    static bool FromSVGString(const char str[], SkPath* outPath) {
        if (auto result = FromSVGString(str)) {
            *outPath = *result;
            return true;
        }
        return false;
    }

    enum class PathEncoding { Absolute, Relative };
    static SkString ToSVGString(const SkPath&, PathEncoding = PathEncoding::Absolute);
};

#endif
