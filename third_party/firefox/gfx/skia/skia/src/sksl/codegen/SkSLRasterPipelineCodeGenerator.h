/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SKSL_RASTERPIPELINECODEGENERATOR)
#define SKSL_RASTERPIPELINECODEGENERATOR

#include "include/core/SkTypes.h"
#include <memory>

namespace SkSL {

class FunctionDefinition;
struct Program;
class DebugTracePriv;
namespace RP { class Program; }

std::unique_ptr<RP::Program> MakeRasterPipelineProgram(const Program& program,
                                                       const FunctionDefinition& function,
                                                       DebugTracePriv* debugTrace = nullptr,
                                                       bool writeTraceOps = false);

}  

#endif
