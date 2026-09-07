/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StartupTimeline.h"
#include "mozilla/TimeStamp.h"
#include "nsXULAppAPI.h"

namespace mozilla {

TimeStamp StartupTimeline::sStartupTimeline[StartupTimeline::MAX_EVENT_ID];
const char*
    StartupTimeline::sStartupTimelineDesc[StartupTimeline::MAX_EVENT_ID] = {
#define mozilla_StartupTimeline_Event(ev, desc) desc,
#include "StartupTimeline.h"
#undef mozilla_StartupTimeline_Event
};

} 

using mozilla::StartupTimeline;
using mozilla::TimeStamp;

void XRE_StartupTimelineRecord(int aEvent, TimeStamp aWhen) {
  StartupTimeline::Record((StartupTimeline::Event)aEvent, aWhen);
}

void StartupTimeline::RecordOnce(Event ev) { RecordOnce(ev, TimeStamp::Now()); }

void StartupTimeline::RecordOnce(Event ev, const TimeStamp& aWhen) {
  if (HasRecord(ev)) {
    return;
  }

  Record(ev, aWhen);

}
