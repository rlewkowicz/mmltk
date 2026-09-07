// Copyright 2013 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_FLAGSTD140STRUCTS_H_)
#define COMPILER_TRANSLATOR_FLAGSTD140STRUCTS_H_

#include <vector>

namespace sh
{

class TField;
class TIntermNode;
class TIntermSymbol;

struct MappedStruct
{
    TIntermSymbol *blockDeclarator;
    TField *field;
};

std::vector<MappedStruct> FlagStd140Structs(TIntermNode *node);
}  

#endif
