// Copyright 2017 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMMON_ALIGNED_MEMORY_H_)
#define COMMON_ALIGNED_MEMORY_H_

#include <cstddef>

namespace angle
{

void *AlignedAlloc(size_t size, size_t alignment);
void AlignedFree(void *ptr);

}  

#endif
