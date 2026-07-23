// Copyright 2002 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEOPS_REMOVEDYNAMICINDEXING_H_)
#define COMPILER_TRANSLATOR_TREEOPS_REMOVEDYNAMICINDEXING_H_

#include "common/angleutils.h"

#include <functional>

namespace sh
{

class TCompiler;
class TIntermNode;
class TIntermBinary;
class TSymbolTable;
class PerformanceDiagnostics;

[[nodiscard]] bool RemoveDynamicIndexingOfNonSSBOVectorOrMatrix(
    TCompiler *compiler,
    TIntermNode *root,
    TSymbolTable *symbolTable,
    PerformanceDiagnostics *perfDiagnostics);

[[nodiscard]] bool RemoveDynamicIndexingOfSwizzledVector(TCompiler *compiler,
                                                         TIntermNode *root,
                                                         TSymbolTable *symbolTable,
                                                         PerformanceDiagnostics *perfDiagnostics);

}  

#endif
