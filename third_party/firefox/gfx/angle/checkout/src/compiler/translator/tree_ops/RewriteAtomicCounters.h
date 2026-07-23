// Copyright 2019 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEOPS_REWRITEATOMICCOUNTERS_H_)
#define COMPILER_TRANSLATOR_TREEOPS_REWRITEATOMICCOUNTERS_H_

#include "common/angleutils.h"

namespace sh
{
class TCompiler;
class TIntermBlock;
class TIntermTyped;
class TSymbolTable;
class TVariable;

[[nodiscard]] bool RewriteAtomicCounters(TCompiler *compiler,
                                         TIntermBlock *root,
                                         TSymbolTable *symbolTable,
                                         const TIntermTyped *acbBufferOffsets,
                                         const TVariable **atomicCountersOut);
}  

#endif
