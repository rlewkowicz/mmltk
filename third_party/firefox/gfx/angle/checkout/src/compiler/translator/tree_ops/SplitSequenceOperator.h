// Copyright 2016 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEOPS_SPLITSEQUENCEOPERATOR_H_)
#define COMPILER_TRANSLATOR_TREEOPS_SPLITSEQUENCEOPERATOR_H_

#include "common/angleutils.h"

namespace sh
{
class TCompiler;
class TIntermNode;
class TSymbolTable;

[[nodiscard]] bool SplitSequenceOperator(TCompiler *compiler,
                                         TIntermNode *root,
                                         int patternsToSplitMask,
                                         TSymbolTable *symbolTable);

}  

#endif
