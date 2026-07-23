/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_ResourceHasher_h
#define mozilla_dom_ResourceHasher_h

#include "nsCOMPtr.h"
#include "nsICryptoHash.h"
#include "nsString.h"

namespace mozilla::dom {

class ResourceHasher final {
 public:
  NS_INLINE_DECL_REFCOUNTING(ResourceHasher)

  static already_AddRefed<ResourceHasher> Init();

  nsresult Update(const uint8_t* aData, uint32_t aLength);

  nsresult Finish();

  const nsACString& GetHash() const { return mComputedHash; }

 private:
  explicit ResourceHasher(nsICryptoHash* aCrypto);
  ~ResourceHasher() = default;

  nsCOMPtr<nsICryptoHash> mCrypto;
  nsCString mComputedHash;
  bool mFinalized;
};

}  

#endif  // mozilla_dom_ResourceHasher_h
