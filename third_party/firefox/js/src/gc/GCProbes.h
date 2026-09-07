/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_GCProbes_h
#define gc_GCProbes_h


#include "gc/AllocKind.h"
#include "js/TraceKind.h"

class JSObject;

namespace js {
namespace gc {

class GCRuntime;
class Cell;

namespace gcprobes {

inline void Init(gc::GCRuntime* gc) {}
inline void Finish(gc::GCRuntime* gc) {}
inline void NurseryAlloc(void* ptr, JS::TraceKind kind) {}
inline void TenuredAlloc(void* ptr, gc::AllocKind kind) {}
inline void CreateObject(JSObject* object) {}
inline void MinorGCStart() {}
inline void PromoteToTenured(gc::Cell* src, gc::Cell* dst) {}
inline void MinorGCEnd() {}
inline void MajorGCStart() {}
inline void TenuredFinalize(gc::Cell* thing) {
}  
inline void MajorGCEnd() {}

}  
}  
}  

#endif  // gc_GCProbes_h
