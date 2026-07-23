// Copyright 2016 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(ANGLEBASE_LOGGING_H_)
#define ANGLEBASE_LOGGING_H_

#include "common/debug.h"

#if !defined(DCHECK)
#    define DCHECK(X) ASSERT(X)
#endif

#if !defined(CHECK)
#    define CHECK(X) ASSERT(X)
#endif

#if !defined(NOTREACHED)
#    define NOTREACHED() ({ UNREACHABLE(); })
#endif

#endif
