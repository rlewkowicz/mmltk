// Copyright 2022 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEOPS_REWRITE_PIXELLOCALSTORAGE_H_)
#define COMPILER_TRANSLATOR_TREEOPS_REWRITE_PIXELLOCALSTORAGE_H_

#include <GLSLANG/ShaderLang.h>

namespace sh
{

class TCompiler;
class TIntermBlock;
class TSymbolTable;

[[nodiscard]] bool RewritePixelLocalStorage(TCompiler *compiler,
                                            TIntermBlock *root,
                                            TSymbolTable &symbolTable,
                                            const ShCompileOptions &compileOptions,
                                            int shaderVersion);

}  

#endif
