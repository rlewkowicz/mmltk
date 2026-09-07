/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Invalidation_h
#define jit_Invalidation_h

#include "jit/IonTypes.h"
#include "js/AllocPolicy.h"
#include "js/GCVector.h"

namespace js {
namespace jit {

class IonScript;

class IonScriptKey {
  JSScript* script_;
  IonCompilationId id_;

 public:
  IonScriptKey(JSScript* script, IonCompilationId id)
      : script_(script), id_(id) {}

  JSScript* script() const { return script_; }

  IonScript* maybeIonScriptToInvalidate() const;

  bool traceWeak(JSTracer* trc);

  bool operator==(const IonScriptKey& other) const {
    return script_ == other.script_ && id_ == other.id_;
  }
  bool operator!=(const IonScriptKey& other) const {
    return !operator==(other);
  }
};

using IonScriptKeyVector = JS::GCVector<IonScriptKey, 1, SystemAllocPolicy>;

void InvalidateAll(JS::GCContext* gcx, JS::Zone* zone);
void FinishInvalidation(JS::GCContext* gcx, JSScript* script);

void AddPendingInvalidation(jit::IonScriptKeyVector& invalid, JSScript* script);

void Invalidate(JSContext* cx, const IonScriptKeyVector& invalid,
                bool resetUses = true, bool cancelOffThread = true);
void Invalidate(JSContext* cx, JSScript* script, bool resetUses = true,
                bool cancelOffThread = true);

}  
}  

#endif /* jit_Invalidation_h */
