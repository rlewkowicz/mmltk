/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_SMIL_SMILMILESTONE_H_
#define DOM_SMIL_SMILMILESTONE_H_

#include "mozilla/SMILTypes.h"

namespace mozilla {

class SMILMilestone {
 public:
  constexpr SMILMilestone(SMILTime aTime, bool aIsEnd)
      : mTime(aTime), mIsEnd(aIsEnd) {}

  constexpr SMILMilestone() : mTime(0), mIsEnd(false) {}

  bool operator==(const SMILMilestone& aOther) const {
    return mTime == aOther.mTime && mIsEnd == aOther.mIsEnd;
  }

  bool operator!=(const SMILMilestone& aOther) const {
    return !(*this == aOther);
  }

  bool operator<(const SMILMilestone& aOther) const {
    return mTime < aOther.mTime ||
           (mTime == aOther.mTime && mIsEnd && !aOther.mIsEnd);
  }

  bool operator<=(const SMILMilestone& aOther) const {
    return *this == aOther || *this < aOther;
  }

  bool operator>=(const SMILMilestone& aOther) const {
    return !(*this < aOther);
  }

  SMILTime mTime;  
  bool mIsEnd;     
};

}  

#endif  // DOM_SMIL_SMILMILESTONE_H_
