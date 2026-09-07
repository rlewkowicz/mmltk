/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_MIDIPortChild_h
#define mozilla_dom_MIDIPortChild_h

#include "mozilla/dom/MIDIPortInterface.h"
#include "mozilla/dom/PMIDIPortChild.h"

namespace mozilla::dom {

class MIDIPort;
class MIDIPortInfo;

class MIDIPortChild final : public PMIDIPortChild, public MIDIPortInterface {
 public:
  NS_INLINE_DECL_REFCOUNTING(MIDIPortChild, override);
  mozilla::ipc::IPCResult RecvReceive(nsTArray<MIDIMessage>&& aMsgs);

  virtual void ActorDestroy(ActorDestroyReason aWhy) override;

  mozilla::ipc::IPCResult RecvUpdateStatus(
      const MIDIPortDeviceState& aDeviceState,
      const MIDIPortConnectionState& aConnectionState);

  MIDIPortChild(const MIDIPortInfo& aPortInfo, bool aSysexEnabled,
                MIDIPort* aPort);
  nsresult GenerateStableId(const nsACString& aOrigin);
  const nsString& StableId() { return mStableId; };

  void DetachOwner() { mDOMPort = nullptr; }

 private:
  ~MIDIPortChild() = default;
  MIDIPort* mDOMPort;
  nsString mStableId;
};
}  

#endif
