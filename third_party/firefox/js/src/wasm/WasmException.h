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

#ifndef wasm_exception_h
#define wasm_exception_h

namespace js {
namespace wasm {

static const uint32_t CatchAllIndex = UINT32_MAX;
static_assert(CatchAllIndex > MaxTags);

struct TryTableCatch {
  TryTableCatch()
      : tagIndex(CatchAllIndex), labelRelativeDepth(0), captureExnRef(false) {}

  uint32_t tagIndex;
  uint32_t labelRelativeDepth;
  bool captureExnRef;
  ValTypeVector labelType;
};
using TryTableCatchVector = Vector<TryTableCatch, 1, SystemAllocPolicy>;

}  
}  

#endif  // wasm_exception_h
