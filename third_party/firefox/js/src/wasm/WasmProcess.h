/*
 * Copyright 2017 Mozilla Foundation
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

#ifndef wasm_process_h
#define wasm_process_h

#include "mozilla/Atomics.h"

#include "wasm/WasmMemory.h"

namespace js {
namespace wasm {

class Code;
class CodeRange;
class CodeBlock;
class TagType;

#ifdef ENABLE_WASM_JSPI
extern const TagType* sJSPromiseTagType;
#endif
extern const TagType* sWrappedJSValueTagType;
static constexpr uint32_t WrappedJSValueTagType_ValueOffset = 0;


const CodeBlock* LookupCodeBlock(const void* pc,
                                 const CodeRange** codeRange = nullptr);

const Code* LookupCode(const void* pc, const CodeRange** codeRange = nullptr);


bool InCompiledCode(void* pc);


extern mozilla::Atomic<bool> CodeExists;


bool RegisterCodeBlock(const CodeBlock* cs);

void UnregisterCodeBlock(const CodeBlock* cs);


bool IsHugeMemoryEnabled(AddressType t, PageSize sz);


bool Init();

void ShutDown();

}  
}  

#endif  // wasm_process_h
