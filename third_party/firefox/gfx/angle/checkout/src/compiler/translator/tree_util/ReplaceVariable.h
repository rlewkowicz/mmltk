// Copyright 2018 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEUTIL_REPLACEVARIABLE_H_)
#define COMPILER_TRANSLATOR_TREEUTIL_REPLACEVARIABLE_H_

#include "common/angleutils.h"
#include "common/hash_containers.h"

namespace sh
{

class TCompiler;
class TIntermBlock;
class TIntermTyped;
class TIntermNode;
class TSymbolTable;
class TSymbolUniqueId;
class TVariable;

[[nodiscard]] bool ReplaceVariable(TCompiler *compiler,
                                   TIntermBlock *root,
                                   const TVariable *toBeReplaced,
                                   const TVariable *replacement);
[[nodiscard]] bool ReplaceVariableWithTyped(TCompiler *compiler,
                                            TIntermBlock *root,
                                            const TVariable *toBeReplaced,
                                            const TIntermTyped *replacement);

using VariableReplacementMap = angle::HashMap<TSymbolUniqueId, const TIntermTyped *>;

[[nodiscard]] bool ReplaceVariables(TCompiler *compiler,
                                    TIntermNode *root,
                                    const VariableReplacementMap &variableMap);

void GetDeclaratorReplacements(TSymbolTable *symbolTable,
                               TIntermBlock *root,
                               VariableReplacementMap *variableMap);

}  

#endif
