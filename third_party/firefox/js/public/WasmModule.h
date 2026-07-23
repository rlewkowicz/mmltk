/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_WasmModule_h
#define js_WasmModule_h

#include "mozilla/RefPtr.h"  // RefPtr

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/RefCounted.h"  // AtomicRefCounted
#include "js/TypeDecls.h"   // HandleObject

namespace JS {


struct WasmModule : js::AtomicRefCounted<WasmModule> {
  virtual ~WasmModule() = default;
  virtual JSObject* createObject(JSContext* cx) const = 0;
};

extern JS_PUBLIC_API bool IsWasmModuleObject(HandleObject obj);

extern JS_PUBLIC_API RefPtr<WasmModule> GetWasmModule(HandleObject obj);

}  

#endif /* js_WasmModule_h */
