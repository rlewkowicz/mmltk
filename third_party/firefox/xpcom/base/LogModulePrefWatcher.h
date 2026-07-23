/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef LogModulePrefWatcher_h
#define LogModulePrefWatcher_h

#include "nsIObserver.h"

namespace mozilla {

class LogModulePrefWatcher : public nsIObserver {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER

  static void RegisterPrefWatcher();

 private:
  LogModulePrefWatcher();
  virtual ~LogModulePrefWatcher() = default;
};
}  

#endif  // LogModulePrefWatcher_h
