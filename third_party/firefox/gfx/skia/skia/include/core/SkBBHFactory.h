/*
 * Copyright 2014 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkBBHFactory_DEFINED)
#define SkBBHFactory_DEFINED

#include "include/core/SkRefCnt.h"
#include "include/core/SkTypes.h"

#include "include/core/SkRect.h"  // IWYU pragma: keep

#include <cstddef>
#include <vector>

class SkBBoxHierarchy : public SkRefCnt {
public:
    struct Metadata {
        bool isDraw;  
    };

    virtual void insert(const SkRect[], int N) = 0;
    virtual void insert(const SkRect[], const Metadata[], int N);

    virtual void search(const SkRect& query, std::vector<int>* results) const = 0;

    virtual size_t bytesUsed() const = 0;

protected:
    SkBBoxHierarchy() = default;
    SkBBoxHierarchy(const SkBBoxHierarchy&) = delete;
    SkBBoxHierarchy& operator=(const SkBBoxHierarchy&) = delete;
};

class SK_API SkBBHFactory {
public:
    virtual sk_sp<SkBBoxHierarchy> operator()() const = 0;
    virtual ~SkBBHFactory() {}

protected:
    SkBBHFactory() = default;
    SkBBHFactory(const SkBBHFactory&) = delete;
    SkBBHFactory& operator=(const SkBBHFactory&) = delete;
};

class SK_API SkRTreeFactory : public SkBBHFactory {
public:
    sk_sp<SkBBoxHierarchy> operator()() const override;
};

#endif
