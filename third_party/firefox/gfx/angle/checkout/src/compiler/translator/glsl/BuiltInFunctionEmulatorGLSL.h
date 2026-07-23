// Copyright 2011 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(COMPILER_TRANSLATOR_GLSL_BUILTINFUNCTIONEMULATORGLSL_H_)
#define COMPILER_TRANSLATOR_GLSL_BUILTINFUNCTIONEMULATORGLSL_H_

#include "GLSLANG/ShaderLang.h"

namespace sh
{
class BuiltInFunctionEmulator;

void InitBuiltInAbsFunctionEmulatorForGLSLWorkarounds(BuiltInFunctionEmulator *emu,
                                                      sh::GLenum shaderType);

void InitBuiltInAtanFunctionEmulatorForGLSLWorkarounds(BuiltInFunctionEmulator *emu);

void InitBuiltInFunctionEmulatorForGLSLMissingFunctions(BuiltInFunctionEmulator *emu,
                                                        sh::GLenum shaderType,
                                                        int targetGLSLVersion);
}  

#endif
