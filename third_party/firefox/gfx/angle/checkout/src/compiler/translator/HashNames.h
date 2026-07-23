// Copyright 2002 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_HASHNAMES_H_)
#define COMPILER_TRANSLATOR_HASHNAMES_H_

#include <map>

#include "GLSLANG/ShaderLang.h"
#include "compiler/translator/Common.h"

namespace sh
{

typedef std::map<TPersistString, TPersistString> NameMap;

class ImmutableString;
class TSymbol;

ImmutableString HashName(const ImmutableString &name,
                         char prefix,
                         ShHashFunction64 hashFunction,
                         NameMap *nameMap);

ImmutableString HashName(const TSymbol *symbol,
                         char prefix,
                         ShHashFunction64 hashFunction,
                         NameMap *nameMap);

}  

#endif
