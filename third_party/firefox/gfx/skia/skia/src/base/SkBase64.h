/*
 * Copyright 2006 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkBase64_DEFINED)
#define SkBase64_DEFINED

#include <cstddef>

struct SkBase64 {
public:
    enum Error {
        kNoError,
        kPadError,
        kBadCharError
    };

    static size_t Encode(const void* src, size_t length, void* dst, const char* encode = nullptr);

    static size_t EncodedSize(size_t srcDataLength) {
        return ((srcDataLength + 2) / 3) * 4;
    }

    [[nodiscard]] static Error Decode(const void* src, size_t  srcLength,
                                      void* dst, size_t* dstLength);
};

#endif
