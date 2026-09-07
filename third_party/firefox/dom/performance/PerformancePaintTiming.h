/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_PerformancePaintTiming_h_
#define mozilla_dom_PerformancePaintTiming_h_

#include "mozilla/dom/PerformanceEntry.h"
#include "mozilla/dom/PerformancePaintTimingBinding.h"

namespace mozilla::dom {

class Performance;

class PerformancePaintTiming final : public PerformanceEntry {
 public:
  PerformancePaintTiming(Performance* aPerformance, const nsAString& aName,
                         const TimeStamp& aStartTime);

  NS_DECL_ISUPPORTS_INHERITED

  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(PerformancePaintTiming,
                                           PerformanceEntry)

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  DOMHighResTimeStamp StartTime() const override;
  DOMHighResTimeStamp PaintTime() const { return StartTime(); }
  Nullable<DOMHighResTimeStamp> GetPresentationTime() const { return nullptr; }

  size_t SizeOfIncludingThis(
      mozilla::MallocSizeOf aMallocSizeOf) const override;

 private:
  ~PerformancePaintTiming();
  RefPtr<Performance> mPerformance;

  const TimeStamp mRawStartTime;
  mutable Maybe<DOMHighResTimeStamp> mCachedStartTime;
};

}  

#endif /* mozilla_dom_PerformanacePaintTiming_h___ */
