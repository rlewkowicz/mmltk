/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_MIDIOutput_h
#define mozilla_dom_MIDIOutput_h

#include "mozilla/dom/MIDIPort.h"

struct JSContext;

namespace mozilla {
class ErrorResult;

namespace dom {

class MIDIPortInfo;
class MIDIMessage;

class MIDIOutput final : public MIDIPort {
 public:
  static RefPtr<MIDIOutput> Create(nsPIDOMWindowInner* aWindow,
                                   MIDIAccess* aMIDIAccessParent,
                                   const MIDIPortInfo& aPortInfo,
                                   const bool aSysexEnabled);
  ~MIDIOutput() = default;

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  void Send(const Sequence<uint8_t>& aData, const Optional<double>& aTimestamp,
            ErrorResult& aRv);
  void Clear();

 private:
  explicit MIDIOutput(nsPIDOMWindowInner* aWindow);
};

}  
}  

#endif  // mozilla_dom_MIDIOutput_h
