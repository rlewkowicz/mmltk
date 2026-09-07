/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ProcessPriorityManager_h_
#define mozilla_ProcessPriorityManager_h_

#include "mozilla/HalTypes.h"

class nsFrameLoader;

namespace mozilla {
namespace dom {
class BrowserParent;
class CanonicalBrowsingContext;
class ContentParent;
}  

class ProcessPriorityManager final {
 public:
  ProcessPriorityManager() = delete;
  ProcessPriorityManager(const ProcessPriorityManager&) = delete;
  const ProcessPriorityManager& operator=(const ProcessPriorityManager&) =
      delete;

  static void Init();

  static void SetProcessPriority(dom::ContentParent* aContentParent,
                                 hal::ProcessPriority aPriority);

  static bool CurrentProcessIsForeground();

  static void BrowserPriorityChanged(dom::CanonicalBrowsingContext* aBC,
                                     bool aPriority);
  static void BrowserPriorityChanged(dom::BrowserParent* aBrowserParent,
                                     bool aPriority);
};

}  

#endif
