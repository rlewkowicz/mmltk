// Copyright 2021 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_TREEOPS_MONOMORPHIZEUNSUPPORTEDFUNCTIONS_H_)
#define COMPILER_TRANSLATOR_TREEOPS_MONOMORPHIZEUNSUPPORTEDFUNCTIONS_H_

#include "common/angleutils.h"
#include "compiler/translator/Compiler.h"

namespace sh
{
class TIntermBlock;
class TSymbolTable;

enum class UnsupportedFunctionArgs
{
    StructContainingSamplers     = 0,
    ArrayOfArrayOfSamplerOrImage = 1,
    AtomicCounter                = 2,
    Image                        = 3,
    PixelLocalStorage            = 4,

    InvalidEnum = 6,
    EnumCount   = 6,
};

using UnsupportedFunctionArgsBitSet = angle::PackedEnumBitSet<UnsupportedFunctionArgs>;

[[nodiscard]] bool MonomorphizeUnsupportedFunctions(TCompiler *compiler,
                                                    TIntermBlock *root,
                                                    TSymbolTable *symbolTable,
                                                    UnsupportedFunctionArgsBitSet);
}  

#endif
