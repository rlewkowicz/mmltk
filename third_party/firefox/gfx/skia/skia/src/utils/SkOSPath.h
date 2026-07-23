/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkOSPath_DEFINED)
#define SkOSPath_DEFINED

#include "include/core/SkString.h"

class SkOSPath {
public:
    static constexpr char SEPARATOR = '/';

    static SkString Join(const char* rootPath, const char* relativePath);

    static SkString Basename(const char* fullPath);

    static SkString Dirname(const char* fullPath);
};

#endif
