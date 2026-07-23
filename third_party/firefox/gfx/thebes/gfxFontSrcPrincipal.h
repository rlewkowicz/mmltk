/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_FONTSRCPRINCIPAL_H
#define MOZILLA_GFX_FONTSRCPRINCIPAL_H

#include "nsCOMPtr.h"
#include "PLDHashTable.h"

class nsIPrincipal;

namespace mozilla {
namespace net {
class nsSimpleURI;
}  
}  

class gfxFontSrcPrincipal {
 public:
  explicit gfxFontSrcPrincipal(nsIPrincipal* aNodePrincipal,
                               nsIPrincipal* aStoragePrincipal);

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(gfxFontSrcPrincipal)

  nsIPrincipal* NodePrincipal() const { return mNodePrincipal; }

  nsIPrincipal* StoragePrincipal() const { return mStoragePrincipal; }

  bool Equals(gfxFontSrcPrincipal* aOther);

  PLDHashNumber Hash() const { return mHash; }

 private:
  ~gfxFontSrcPrincipal();

  nsCOMPtr<nsIPrincipal> mNodePrincipal;

  nsCOMPtr<nsIPrincipal> mStoragePrincipal;

  PLDHashNumber mHash;
};

#endif  // MOZILLA_GFX_FONTSRCPRINCIPAL_H
