/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef dom_performance_PerformanceService_h
#define dom_performance_PerformanceService_h

#include "mozilla/TimeStamp.h"
#include "nsCOMPtr.h"
#include "nsDOMNavigationTiming.h"

namespace mozilla::dom {



class PerformanceService {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(PerformanceService)

  static PerformanceService* GetOrCreate();

  DOMHighResTimeStamp TimeOrigin(const TimeStamp& aCreationTimeStamp) const;

 private:
  PerformanceService();
  ~PerformanceService() = default;

  TimeStamp mCreationTimeStamp;
  PRTime mCreationEpochTime;
};

}  

#endif  // dom_performance_PerformanceService_h
