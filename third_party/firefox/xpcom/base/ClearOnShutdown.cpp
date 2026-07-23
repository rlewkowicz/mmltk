/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ClearOnShutdown.h"

namespace mozilla {
namespace ClearOnShutdown_Internal {

Array<StaticAutoPtr<ShutdownList>,
      static_cast<size_t>(ShutdownPhase::ShutdownPhase_Length)>
    sShutdownObservers;
ShutdownPhase sCurrentClearOnShutdownPhase = ShutdownPhase::NotInShutdown;

void InsertIntoShutdownList(ShutdownObserver* aObserver, ShutdownPhase aPhase) {
  if (PastShutdownPhase(aPhase)) {
    MOZ_ASSERT(false, "ClearOnShutdown for phase that already was cleared");
    aObserver->Shutdown();
    delete aObserver;
    return;
  }

  if (!(sShutdownObservers[static_cast<size_t>(aPhase)])) {
    sShutdownObservers[static_cast<size_t>(aPhase)] = new ShutdownList();
  }
  sShutdownObservers[static_cast<size_t>(aPhase)]->insertBack(aObserver);
}

}  

void KillClearOnShutdown(ShutdownPhase aPhase) {
  using namespace ClearOnShutdown_Internal;

  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!PastShutdownPhase(aPhase));

  sCurrentClearOnShutdownPhase = aPhase;

  for (size_t phase = static_cast<size_t>(ShutdownPhase::First);
       phase <= static_cast<size_t>(aPhase); phase++) {
    if (sShutdownObservers[static_cast<size_t>(phase)]) {
      while (ShutdownObserver* observer =
                 sShutdownObservers[static_cast<size_t>(phase)]->popLast()) {
        observer->Shutdown();
        delete observer;
      }
      sShutdownObservers[static_cast<size_t>(phase)] = nullptr;
    }
  }
}

}  
