/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_DigitalCredentialChild_h
#define mozilla_dom_DigitalCredentialChild_h

#include "mozilla/dom/PDigitalCredential.h"
#include "mozilla/dom/PDigitalCredentialChild.h"

namespace mozilla::dom {

class DigitalCredentialHandler;

class DigitalCredentialChild final : public PDigitalCredentialChild {
 public:
  NS_INLINE_DECL_REFCOUNTING(DigitalCredentialChild, override);

  DigitalCredentialChild() = default;

  void ActorDestroy(ActorDestroyReason why) override;

  void SetHandler(DigitalCredentialHandler* aHandler);

 private:
  ~DigitalCredentialChild() = default;

  DigitalCredentialHandler* mHandler{};
};

}  

#endif  // mozilla_dom_DigitalCredentialChild_h
