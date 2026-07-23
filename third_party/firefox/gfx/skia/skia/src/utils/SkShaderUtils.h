/*
 * Copyright 2019 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkShaderUtils_DEFINED)
#define SkShaderUtils_DEFINED

#include "include/core/SkSpan.h"
#include "include/private/base/SkDebug.h"

#include <cstdint>
#include <functional>
#include <string>

namespace SkSL { enum class ProgramKind : int8_t; }

namespace SkShaderUtils {

std::string PrettyPrint(const std::string& string);

void VisitLineByLine(const std::string& text,
                     const std::function<void(int lineNumber, const char* lineText)>&);

inline void PrintLineByLine(const std::string& text) {
    VisitLineByLine(text, [](int lineNumber, const char* lineText) {
        SkDebugf("%4i\t%s\n", lineNumber, lineText);
    });
}

std::string SpirvAsHexStream(SkSpan<const uint32_t> spirv);

std::string BuildShaderErrorMessage(const char* shader, const char* errors);

void PrintShaderBanner(SkSL::ProgramKind programKind);

}  

#endif
