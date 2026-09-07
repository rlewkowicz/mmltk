// Copyright 2024 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_NULL_TRANSLATORNULL_H_)
#define COMPILER_TRANSLATOR_NULL_TRANSLATORNULL_H_

#include "compiler/translator/Compiler.h"

namespace sh
{

class TranslatorNULL : public TCompiler
{
  public:
    TranslatorNULL(sh::GLenum type, ShShaderSpec spec) : TCompiler(type, spec, SH_NULL_OUTPUT) {}

  protected:
    [[nodiscard]] bool translate(TIntermBlock *root,
                                 const ShCompileOptions &compileOptions,
                                 PerformanceDiagnostics *perfDiagnostics) override
    {
        getInfoSink().obj << "\n";
        return true;
    }

    bool shouldFlattenPragmaStdglInvariantAll() override { return false; }
};

}  

#endif
