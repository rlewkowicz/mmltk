// Copyright 2002 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_VARIABLEPACKER_H_)
#define COMPILER_TRANSLATOR_VARIABLEPACKER_H_

#include <vector>

#include <GLSLANG/ShaderLang.h>

namespace sh
{

int GetTypePackingComponentsPerRow(sh::GLenum type);

int GetTypePackingRows(sh::GLenum type);

bool CheckVariablesInPackingLimits(unsigned int maxVectors,
                                   const std::vector<ShaderVariable> &variables);

}  

#endif
