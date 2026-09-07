/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_backgroundchild_h_
#define mozilla_ipc_backgroundchild_h_

#include "mozilla/dom/ProcessIsolation.h"

class nsIEventTarget;

namespace mozilla {
namespace dom {

class BlobImpl;
class ContentChild;
class ContentParent;
class ContentProcess;

}  

namespace ipc {

class PBackgroundChild;
class PBackgroundStarterChild;

class BackgroundChild final {
  friend class mozilla::dom::ContentParent;
  friend class mozilla::dom::ContentProcess;

 public:
  static PBackgroundChild* GetForCurrentThread();

  static PBackgroundChild* GetOrCreateForCurrentThread();

  static void CloseForCurrentThread();

  static void InitContentStarter(mozilla::dom::ContentChild* aContent);

  static bool ValidatePrincipal(
      nsIPrincipal* aPrincipal,
      const EnumSet<dom::ValidatePrincipalOptions>& aOptions);
  static bool ValidatePrincipalInfo(
      const PrincipalInfo& aPrincipalInfo,
      const EnumSet<dom::ValidatePrincipalOptions>& aOptions);

 private:
  static void Startup();
};

}  
}  

#endif  // mozilla_ipc_backgroundchild_h_
