/*
 * Copyright 2015 Mozilla Foundation
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

#ifndef wasm_compile_h
#define wasm_compile_h

#include "vm/Runtime.h"
#include "wasm/WasmComponent.h"
#include "wasm/WasmModule.h"

namespace JS {
class OptimizedEncodingListener;
}

namespace js {
namespace wasm {

class Code;

#ifdef ENABLE_WASM_COMPONENTS
using SharedModuleOrComponent =
    mozilla::Maybe<mozilla::Variant<SharedModule, SharedComponent>>;
#endif

uint32_t ObservedCPUFeatures();

double EstimateCompiledCodeSize(Tier tier, size_t bytecodeSize);

SharedModule CompileModule(const CompileArgs& args,
                           const BytecodeBufferOrSource& bytecode,
                           UniqueChars* error, UniqueCharsVector* warnings,
                           JS::OptimizedEncodingListener* listener = nullptr);

#ifdef ENABLE_WASM_COMPONENTS
SharedComponent CompileComponent(
    const CompileArgs& args, const BytecodeBufferOrSource& bytecode,
    UniqueChars* error, UniqueCharsVector* warnings,
    JS::OptimizedEncodingListener* listener = nullptr);

SharedModuleOrComponent CompileBuffer(
    const CompileArgs& args, const BytecodeBufferOrSource& bytecode,
    UniqueChars* error, UniqueCharsVector* warnings,
    JS::OptimizedEncodingListener* listener = nullptr);
#endif

bool CompileCompleteTier2(const ShareableBytes* codeSection,
                          const Module& module, UniqueChars* error,
                          UniqueCharsVector* warnings,
                          mozilla::Atomic<bool>* cancelled);

bool CompilePartialTier2(const Code& code, uint32_t funcIndex,
                         UniqueChars* error, UniqueCharsVector* warnings,
                         mozilla::Atomic<bool>* cancelled);

using ExclusiveBytesPtr = ExclusiveWaitableData<const uint8_t*>;

struct StreamEndData {
  bool reached;
  const ShareableBytes* tailBytes;
  CompleteTier2Listener completeTier2Listener;

  StreamEndData() : reached(false), tailBytes(nullptr) {}
};
using ExclusiveStreamEndData = ExclusiveWaitableData<StreamEndData>;

SharedModule CompileStreaming(const CompileArgs& args,
                              const ShareableBytes& envBytes,
                              const ShareableBytes& codeBytes,
                              const ExclusiveBytesPtr& codeBytesEnd,
                              const ExclusiveStreamEndData& streamEnd,
                              const mozilla::Atomic<bool>& cancelled,
                              UniqueChars* error, UniqueCharsVector* warnings);

bool DumpIonFunctionInModule(const ShareableBytes& bytecode,
                             uint32_t targetFuncIndex, GenericPrinter& out,
                             UniqueChars* error);

}  
}  

#endif  // namespace wasm_compile_h
