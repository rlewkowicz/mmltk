/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_LoginDetectionService_h
#define mozilla_dom_LoginDetectionService_h

#include "mozilla/Logging.h"
#include "mozilla/Preferences.h"
#include "nsILoginDetectionService.h"
#include "nsILoginManager.h"
#include "nsIObserver.h"
#include "nsIObserverService.h"
#include "nsWeakReference.h"

namespace mozilla::dom {

class LoginDetectionService final : public nsILoginDetectionService,
                                    public nsILoginSearchCallback,
                                    public nsIObserver,
                                    public nsSupportsWeakReference {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSILOGINDETECTIONSERVICE
  NS_DECL_NSILOGINSEARCHCALLBACK
  NS_DECL_NSIOBSERVER

  static already_AddRefed<LoginDetectionService> GetSingleton();

  void MaybeStartMonitoring();

 private:
  LoginDetectionService();
  virtual ~LoginDetectionService();

  void FetchLogins();

  void RegisterObserver();
  void UnregisterObserver();

  nsCOMPtr<nsIObserverService> mObs;

  bool mIsLoginsLoaded;
};

}  

#endif
