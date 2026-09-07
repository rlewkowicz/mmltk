/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_MIDIInput_h
#define mozilla_dom_MIDIInput_h

#include "mozilla/dom/MIDIPort.h"

struct JSContext;

namespace mozilla::dom {

class MIDIPortInfo;

class MIDIInput final : public MIDIPort {
 public:
  static RefPtr<MIDIInput> Create(nsPIDOMWindowInner* aWindow,
                                  MIDIAccess* aMIDIAccessParent,
                                  const MIDIPortInfo& aPortInfo,
                                  const bool aSysexEnabled);
  ~MIDIInput() = default;

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  IMPL_EVENT_HANDLER(midimessage);

  void StateChange() override;
  void EventListenerAdded(nsAtom* aType) override;
  void DisconnectFromOwner() override;

 private:
  explicit MIDIInput(nsPIDOMWindowInner* aWindow);
  void Receive(const nsTArray<MIDIMessage>& aMsgs) override;

  void KeepAliveOnMidimessage();
  void DontKeepAliveOnMidimessage();

  bool mKeepAlive;
};

}  

#endif  // mozilla_dom_MIDIInput_h
