// Copyright 2018 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEOPS_GLSL_REWRITEREPEATEDASSIGNTOSWIZZLED_H_)
#define COMPILER_TRANSLATOR_TREEOPS_GLSL_REWRITEREPEATEDASSIGNTOSWIZZLED_H_

#include "common/angleutils.h"
#include "common/debug.h"

namespace sh
{

class TCompiler;
class TIntermBlock;

#if defined(ANGLE_ENABLE_GLSL)
[[nodiscard]] bool RewriteRepeatedAssignToSwizzled(TCompiler *compiler, TIntermBlock *root);
#else
[[nodiscard]] ANGLE_INLINE bool RewriteRepeatedAssignToSwizzled(TCompiler *compiler,
                                                                TIntermBlock *root)
{
    UNREACHABLE();
    return false;
}
#endif

}  

#endif
