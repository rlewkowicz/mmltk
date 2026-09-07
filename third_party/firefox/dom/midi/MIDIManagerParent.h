/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_MIDIManagerParent_h
#define mozilla_dom_MIDIManagerParent_h

#include "mozilla/dom/PMIDIManagerParent.h"

namespace mozilla::dom {

class MIDIManagerParent final : public PMIDIManagerParent {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(MIDIManagerParent, override)
  MIDIManagerParent() = default;
  mozilla::ipc::IPCResult RecvRefresh();
  mozilla::ipc::IPCResult RecvShutdown();
  void ActorDestroy(ActorDestroyReason aWhy) override;

 private:
  ~MIDIManagerParent() = default;
};

}  

#endif  // mozilla_dom_MIDIManagerParent_h
