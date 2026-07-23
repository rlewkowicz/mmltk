/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_MIDIMessageQueue_h
#define mozilla_dom_MIDIMessageQueue_h

#include "mozilla/Mutex.h"
#include "nsTArray.h"

#include "mozilla/dom/MIDITypes.h"

namespace mozilla {

class TimeStamp;

namespace dom {

class MIDIMessage;

class MIDIMessageQueue {
 public:
  MIDIMessageQueue();
  ~MIDIMessageQueue() = default;
  void Add(nsTArray<MIDIMessage>& aMsg);
  void GetMessagesBefore(TimeStamp aTimestamp,
                         nsTArray<MIDIMessage>& aMsgQueue);
  void GetMessages(nsTArray<MIDIMessage>& aMsgQueue);
  void Clear();
  void ClearAfterNow();

 private:
  nsTArray<MIDIMessage> mMessageQueue;
  Mutex mMutex MOZ_UNANNOTATED;
};

}  
}  

#endif  // mozilla_dom_MIDIMessageQueue_h
