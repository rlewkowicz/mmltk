/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DDLogMessage_h_
#define DDLogMessage_h_

#include "DDLogCategory.h"
#include "DDLogObject.h"
#include "DDLogValue.h"
#include "DDMessageIndex.h"
#include "DDTimeStamp.h"
#include "nsString.h"

namespace mozilla {

class DDLifetimes;

struct DDLogMessage {
  DDMessageIndex mIndex;
  DDTimeStamp mTimeStamp;
  DDLogObject mObject;
  DDLogCategory mCategory;
  const char* mLabel;
  DDLogValue mValue = DDLogValue{DDNoValue{}};

  nsCString Print() const;

  nsCString Print(const DDLifetimes& aLifetimes) const;
};

}  

#endif  // DDLogMessage_h_
