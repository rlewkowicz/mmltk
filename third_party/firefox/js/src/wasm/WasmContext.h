/*
 * Copyright 2020 Mozilla Foundation
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

#if !defined(wasm_context_h)
#define wasm_context_h

#if defined(ENABLE_WASM_JSPI)
#  include "wasm/WasmStacks.h"
#endif

#include "js/AllocPolicy.h"
#include "js/NativeStackLimits.h"


namespace js::wasm {

struct Handlers;
class ContObject;
class ContStack;
class ContStackArena;


class Context {
 public:
  Context();
  ~Context();

  static constexpr size_t offsetOfStackLimit() {
    return offsetof(Context, stackLimit);
  }
  void initStackLimit(JSContext* cx);

#if defined(ENABLE_WASM_JSPI)
  static constexpr size_t offsetOfCurrentStack() {
    return offsetof(Context, currentStack_);
  }
  static constexpr size_t offsetOfBaseHandlers() {
    return offsetof(Context, baseHandlers_);
  }
  static constexpr size_t offsetOfMainStackTarget() {
    return offsetof(Context, mainStackTarget_);
  }

  ContStack* currentStack() { return currentStack_; }
  Handlers* baseHandlers() { return baseHandlers_; }
  bool onContStack() const { return currentStack_ != nullptr; }
  ContStackAllocator& contStacks() { return contStacks_; }
  const ContStackAllocator& contStacks() const { return contStacks_; }

  const StackTarget& mainStackTarget() const { return mainStackTarget_; }

  ContStack* findStackForAddress(JSContext* cx, uintptr_t stackAddress);
#endif

  bool triedToInstallSignalHandlers;
  bool haveSignalHandlers;

  JS::NativeStackLimit stackLimit;

 private:
#if defined(ENABLE_WASM_JSPI)
  StackTarget mainStackTarget_;


  ContStack* currentStack_;
  Handlers* baseHandlers_;

  ContStackAllocator contStacks_;
#endif
};

}  

#endif
