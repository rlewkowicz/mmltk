/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_contentblockinguserinteraction_h
#define mozilla_contentblockinguserinteraction_h

#define USER_INTERACTION_PERM "storageAccessAPI"_ns

class nsIPrincipal;

namespace mozilla {

class ContentBlockingUserInteraction final {
 public:
  static void Observe(nsIPrincipal* aPrincipal);

  static bool Exists(nsIPrincipal* aPrincipal);
};

}  

#endif  // mozilla_contentblockinguserinteraction_h
