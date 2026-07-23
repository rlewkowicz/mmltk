/*
 * Copyright 2021 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SKSL_DEBUG_TRACE)
#define SKSL_DEBUG_TRACE

#include "include/core/SkRefCnt.h"

class SkWStream;

namespace SkSL {

class DebugTrace : public SkRefCnt {
public:
    virtual void dump(SkWStream* o) const = 0;
};

} 

#endif
