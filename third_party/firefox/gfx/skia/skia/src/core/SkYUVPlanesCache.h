/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkYUVPlanesCache_DEFINED)
#define SkYUVPlanesCache_DEFINED

#include "include/core/SkTypes.h"

#include <cstdint>

class SkCachedData;
class SkResourceCache;
class SkYUVAPixmaps;

class SkYUVPlanesCache {
public:
    static SkCachedData* FindAndRef(uint32_t genID,
                                    SkYUVAPixmaps* pixmaps,
                                    SkResourceCache* localCache = nullptr);

    static void Add(uint32_t genID, SkCachedData* data, const SkYUVAPixmaps& pixmaps,
                    SkResourceCache* localCache = nullptr);
};

#endif
