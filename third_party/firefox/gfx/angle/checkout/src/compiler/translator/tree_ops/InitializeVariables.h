// Copyright 2002 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEOPS_INITIALIZEVARIABLES_H_)
#define COMPILER_TRANSLATOR_TREEOPS_INITIALIZEVARIABLES_H_

#include <GLSLANG/ShaderLang.h>

#include "compiler/translator/ExtensionBehavior.h"
#include "compiler/translator/IntermNode.h"

namespace sh
{
class TCompiler;
class TSymbolTable;

typedef std::vector<const TVariable *> InitVariableList;


void CreateInitCode(const TIntermTyped *initializedSymbol,
                    bool canUseLoopsToInitialize,
                    TIntermSequence *initCode,
                    TSymbolTable *symbolTable);

[[nodiscard]] bool InitializeUninitializedLocals(TCompiler *compiler,
                                                 TIntermBlock *root,
                                                 int shaderVersion,
                                                 bool canUseLoopsToInitialize,
                                                 TSymbolTable *symbolTable);

[[nodiscard]] bool InitializeVariables(TCompiler *compiler,
                                       TIntermBlock *root,
                                       const InitVariableList &vars,
                                       TSymbolTable *symbolTable,
                                       int shaderVersion,
                                       const TExtensionBehavior &extensionBehavior,
                                       bool canUseLoopsToInitialize);

}  

#endif
