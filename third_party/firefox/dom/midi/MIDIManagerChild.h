/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_MIDIManagerChild_h
#define mozilla_dom_MIDIManagerChild_h

#include "mozilla/dom/PMIDIManagerChild.h"

namespace mozilla::dom {

class MIDIManagerChild final : public PMIDIManagerChild {
 public:
  NS_INLINE_DECL_REFCOUNTING(MIDIManagerChild)

  MIDIManagerChild();
  mozilla::ipc::IPCResult RecvMIDIPortListUpdate(const MIDIPortList& aPortList);
  void Shutdown();

 private:
  ~MIDIManagerChild() = default;
  bool mShutdown;
};

}  

#endif  // mozilla_dom_MIDIManagerChild_h
