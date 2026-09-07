/*
 * Copyright 2021 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_codegen_constants_h
#define wasm_codegen_constants_h

#include <stdint.h>

namespace js {
namespace wasm {

static const unsigned MaxArgsForJitInlineCall = 8;
static const unsigned MaxResultsForJitEntry = 1;
static const unsigned MaxResultsForJitExit = 1;
static const unsigned MaxResultsForJitInlineCall = MaxResultsForJitEntry;

static const unsigned MaxFieldsScalarReplacementStructs = 10;

static const unsigned MaxRegisterResults = 1;

static const unsigned InterpFailInstanceReg = 0xbad;


#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM64)
static const uint32_t MaxInlineMemoryCopyLength = 64;
static const uint32_t MaxInlineMemoryFillLength = 64;
#elif defined(JS_CODEGEN_X86)
static const uint32_t MaxInlineMemoryCopyLength = 32;
static const uint32_t MaxInlineMemoryFillLength = 32;
#else
static const uint32_t MaxInlineMemoryCopyLength = 1;
static const uint32_t MaxInlineMemoryFillLength = 1;
#endif

static const uint32_t MinSuperTypeVectorLength = 8;

static const uint32_t JumpTableJitEntryOffset = 0;

#define STATIC_ASSERT_WASM_FUNCTIONS_TENURED

}  
}  

#endif  // wasm_codegen_constants_h
