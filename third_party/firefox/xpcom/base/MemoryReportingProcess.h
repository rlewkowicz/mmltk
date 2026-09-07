/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef xpcom_base_MemoryReportingProcess_h
#define xpcom_base_MemoryReportingProcess_h

#include <stdint.h>
#include "nscore.h"

namespace mozilla {
namespace ipc {
class FileDescriptor;
}  

template <class T>
class Maybe;

class MemoryReportingProcess {
 public:
  NS_IMETHOD_(MozExternalRefCountType) AddRef() = 0;
  NS_IMETHOD_(MozExternalRefCountType) Release() = 0;

  virtual ~MemoryReportingProcess() = default;

  virtual bool IsAlive() const = 0;

  virtual bool SendRequestMemoryReport(
      const uint32_t& aGeneration, const bool& aAnonymize,
      const bool& aMinimizeMemoryUsage,
      const Maybe<mozilla::ipc::FileDescriptor>& aDMDFile) = 0;

  virtual int32_t Pid() const = 0;
};

}  

#endif  // xpcom_base_MemoryReportingProcess_h
