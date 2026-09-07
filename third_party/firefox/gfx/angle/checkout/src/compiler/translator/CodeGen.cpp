// Copyright 2013 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compiler/translator/null/TranslatorNULL.h"

#if defined(ANGLE_ENABLE_ESSL)
#    include "compiler/translator/glsl/TranslatorESSL.h"
#endif

#if defined(ANGLE_ENABLE_GLSL)
#    include "compiler/translator/glsl/TranslatorGLSL.h"
#endif


#if defined(ANGLE_ENABLE_VULKAN)
#    include "compiler/translator/spirv/TranslatorSPIRV.h"
#endif


#if defined(ANGLE_ENABLE_WGPU)
#    include "compiler/translator/wgsl/TranslatorWGSL.h"
#endif

#include "compiler/translator/util.h"

namespace sh
{

TCompiler *ConstructCompiler(sh::GLenum type, ShShaderSpec spec, ShShaderOutput output)
{
    if (IsOutputNULL(output))
    {
        return new TranslatorNULL(type, spec);
    }

#if defined(ANGLE_ENABLE_ESSL)
    if (IsOutputESSL(output))
    {
        return new TranslatorESSL(type, spec);
    }
#endif

#if defined(ANGLE_ENABLE_GLSL)
    if (IsOutputGLSL(output))
    {
        return new TranslatorGLSL(type, spec, output);
    }
#endif


#if defined(ANGLE_ENABLE_VULKAN)
    if (IsOutputSPIRV(output))
    {
        return new TranslatorSPIRV(type, spec);
    }
#endif


#if defined(ANGLE_ENABLE_WGPU)
    if (IsOutputWGSL(output))
    {
        return new TranslatorWGSL(type, spec, output);
    }
#endif

    return nullptr;
}

void DeleteCompiler(TCompiler *compiler)
{
    SafeDelete(compiler);
}

}  
