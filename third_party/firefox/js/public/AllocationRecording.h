/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_AllocationRecording_h
#define js_AllocationRecording_h

#include <stdint.h>
#include "jstypes.h"
#include "js/TypeDecls.h"

namespace JS {

struct RecordAllocationInfo {
  RecordAllocationInfo(const char16_t* typeName, const char* className,
                       const char16_t* descriptiveTypeName,
                       const char* coarseType, uint64_t size, bool inNursery)
      : typeName(typeName),
        className(className),
        descriptiveTypeName(descriptiveTypeName),
        coarseType(coarseType),
        size(size),
        inNursery(inNursery) {}

  const char16_t* typeName;
  const char* className;
  const char16_t* descriptiveTypeName;

  const char* coarseType;

  uint64_t size;

  bool inNursery;
};

typedef void (*RecordAllocationsCallback)(RecordAllocationInfo&& info);

JS_PUBLIC_API void EnableRecordingAllocations(
    JSContext* cx, RecordAllocationsCallback callback, double probability);

JS_PUBLIC_API void DisableRecordingAllocations(JSContext* cx);

}  

#endif /* js_AllocationRecording_h */
