/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef LAYOUT_STYLE_TYPEDOM_CSSUNSUPPORTEDVALUE_H_
#define LAYOUT_STYLE_TYPEDOM_CSSUNSUPPORTEDVALUE_H_

#include "mozilla/CSSPropertyId.h"
#include "mozilla/RefPtr.h"
#include "mozilla/dom/CSSStyleValue.h"

template <class T>
class nsCOMPtr;
class nsISupports;

namespace mozilla {

class DeclarationBlock;
struct StyleUnsupportedValue;

namespace dom {

class CSSUnsupportedValue final : public CSSStyleValue {
 public:
  CSSUnsupportedValue(nsCOMPtr<nsISupports> aParent,
                      const CSSPropertyId& aPropertyId,
                      RefPtr<DeclarationBlock> aDeclarations);

  static RefPtr<CSSUnsupportedValue> Create(
      nsCOMPtr<nsISupports> aParent, const CSSPropertyId& aPropertyId,
      StyleUnsupportedValue&& aUnsupportedValue);

  const CSSPropertyId& GetPropertyId() const { return mPropertyId; }

  CSSPropertyId& GetPropertyId() { return mPropertyId; }

  void ToCssTextWithProperty(const CSSPropertyId& aPropertyId,
                             nsACString& aDest) const;

 private:
  virtual ~CSSUnsupportedValue() = default;

  CSSPropertyId mPropertyId;
  RefPtr<DeclarationBlock> mDeclarations;
};

}  
}  

#endif  // LAYOUT_STYLE_TYPEDOM_CSSUNSUPPORTEDVALUE_H_
