/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkPixelStorage_DEFINED)
#define SkPixelStorage_DEFINED

#include <cstdint>

class SkPixelStorage {
public:
    SkPixelStorage();
    virtual ~SkPixelStorage() = default;

    enum Type {
        kTextureProxy,
        kPixelRef,
    };

    uint32_t getId() const { return fID; }
    virtual Type type() const = 0;

private:
    uint32_t fID;

    static uint32_t NextId();
};

#endif
