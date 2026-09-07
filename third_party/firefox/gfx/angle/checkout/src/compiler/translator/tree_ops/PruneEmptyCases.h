// Copyright 2018 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEOPS_PRUNEEMPTYCASES_H_)
#define COMPILER_TRANSLATOR_TREEOPS_PRUNEEMPTYCASES_H_

#include "common/angleutils.h"

namespace sh
{
class TCompiler;
class TIntermBlock;

[[nodiscard]] bool PruneEmptyCases(TCompiler *compiler, TIntermBlock *root);
}  

#endif
