/*
 * Copyright 2016 Mozilla Foundation
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

#ifndef wasm_realm_h
#define wasm_realm_h

#include "js/TracingAPI.h"

#include "wasm/WasmTypeDecls.h"

namespace js {

class WasmTagObject;

namespace wasm {


class Realm {
  JSRuntime* runtime_;
  InstanceVector instances_;

 public:
  explicit Realm(JSRuntime* rt);
  ~Realm();


  bool registerInstance(JSContext* cx, Handle<WasmInstanceObject*> instanceObj);
  void unregisterInstance(Instance& instance);


  const InstanceVector& instances() const { return instances_; }


  void ensureProfilingLabels(bool profilingEnabled);


  void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                              size_t* realmTables);
};


extern void InterruptRunningCode(JSContext* cx);


void ResetInterruptState(JSContext* cx);

}  
}  

#endif  // wasm_realm_h
