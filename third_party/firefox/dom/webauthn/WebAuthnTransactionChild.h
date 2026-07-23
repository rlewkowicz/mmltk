/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_WebAuthnTransactionChild_h
#define mozilla_dom_WebAuthnTransactionChild_h

#include "mozilla/dom/PWebAuthnTransactionChild.h"


namespace mozilla::dom {

class WebAuthnHandler;

class WebAuthnTransactionChild final : public PWebAuthnTransactionChild {
 public:
  NS_INLINE_DECL_REFCOUNTING(WebAuthnTransactionChild, override);

  WebAuthnTransactionChild() = default;

  void ActorDestroy(ActorDestroyReason why) override;

  void SetHandler(WebAuthnHandler* aMananger);

 private:
  ~WebAuthnTransactionChild() = default;

  WebAuthnHandler* mHandler;
};

}  

#endif  // mozilla_dom_WebAuthnTransactionChild_h
