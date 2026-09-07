/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef dom_ipc_MemMapSnapshot_h
#define dom_ipc_MemMapSnapshot_h

#include "ErrorList.h"
#include "mozilla/Attributes.h"
#include "mozilla/RangedPtr.h"
#include "mozilla/Result.h"
#include "mozilla/ipc/SharedMemoryMapping.h"

namespace mozilla::ipc {

class MOZ_RAII MemMapSnapshot {
 public:
  Result<Ok, nsresult> Init(size_t aSize);
  Result<ReadOnlySharedMemoryHandle, nsresult> Finalize();

  template <typename T>
  RangedPtr<T> Get() {
    MOZ_ASSERT(mMem);
    auto span = mMem.DataAsSpan<T>();
    return {span.data(), span.size()};
  }

 private:
  FreezableSharedMemoryMapping mMem;
};

}  

#endif  // dom_ipc_MemMapSnapshot_h
