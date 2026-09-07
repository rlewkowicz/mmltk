// Copyright 2016 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEOPS_SIMPLIFYLOOPCONDITIONS_H_)
#define COMPILER_TRANSLATOR_TREEOPS_SIMPLIFYLOOPCONDITIONS_H_

#include "common/angleutils.h"

namespace sh
{
class TCompiler;
class TIntermNode;
class TSymbolTable;

[[nodiscard]] bool SimplifyLoopConditions(TCompiler *compiler,
                                          TIntermNode *root,
                                          TSymbolTable *symbolTable);

[[nodiscard]] bool SimplifyLoopConditions(TCompiler *compiler,
                                          TIntermNode *root,
                                          unsigned int conditionsToSimplify,
                                          TSymbolTable *symbolTable);
}  

#endif
