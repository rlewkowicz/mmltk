// Copyright 2025 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEOPS_GATHERDEFAULTUNIFORMS_H_)
#define COMPILER_TRANSLATOR_TREEOPS_GATHERDEFAULTUNIFORMS_H_

#include "common/PackedGLEnums_autogen.h"
#include "compiler/translator/ImmutableString.h"

namespace sh
{
class TCompiler;
class TIntermBlock;
class TSymbolTable;
class TVariable;
class TType;

[[nodiscard]] bool IsDefaultUniform(const TType &type);

[[nodiscard]] bool GatherDefaultUniforms(TCompiler *compiler,
                                         TIntermBlock *root,
                                         TSymbolTable *symbolTable,
                                         gl::ShaderType shaderType,
                                         const ImmutableString &uniformBlockType,
                                         const ImmutableString &uniformBlockVarName,
                                         const TVariable **outUniformBlock);

}  

#endif
