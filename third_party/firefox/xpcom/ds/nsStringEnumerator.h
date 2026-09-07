/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsStringEnumerator_h
#define nsStringEnumerator_h

#include "nsIStringEnumerator.h"
#include "nsStringFwd.h"
#include "nsTArrayForwardDeclare.h"

class nsStringEnumeratorBase : public nsIStringEnumerator,
                               public nsIUTF8StringEnumerator {
 public:
  NS_DECL_NSISTRINGENUMERATORBASE

  NS_IMETHOD GetNext(nsAString&) override;

  using nsIUTF8StringEnumerator::GetNext;

 protected:
  virtual ~nsStringEnumeratorBase() = default;
};




[[nodiscard]] nsresult NS_NewStringEnumerator(nsIStringEnumerator** aResult,
                                              const nsTArray<nsString>* aArray,
                                              nsISupports* aOwner);
[[nodiscard]] nsresult NS_NewUTF8StringEnumerator(
    nsIUTF8StringEnumerator** aResult, const nsTArray<nsCString>* aArray);

[[nodiscard]] nsresult NS_NewStringEnumerator(nsIStringEnumerator** aResult,
                                              const nsTArray<nsString>* aArray);

[[nodiscard]] nsresult NS_NewAdoptingStringEnumerator(
    nsIStringEnumerator** aResult, nsTArray<nsString>* aArray);

[[nodiscard]] nsresult NS_NewAdoptingUTF8StringEnumerator(
    nsIUTF8StringEnumerator** aResult, nsTArray<nsCString>* aArray);

[[nodiscard]] nsresult NS_NewUTF8StringEnumerator(
    nsIUTF8StringEnumerator** aResult, const nsTArray<nsCString>* aArray,
    nsISupports* aOwner);

#endif  // defined nsStringEnumerator_h
