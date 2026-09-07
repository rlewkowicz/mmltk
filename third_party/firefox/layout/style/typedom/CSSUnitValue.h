/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef LAYOUT_STYLE_TYPEDOM_CSSUNITVALUE_H_
#define LAYOUT_STYLE_TYPEDOM_CSSUNITVALUE_H_

#include "js/TypeDecls.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/dom/CSSNumericValue.h"
#include "nsString.h"

template <class T>
struct already_AddRefed;
template <class T>
class nsCOMPtr;
class nsISupports;
template <class T>
class RefPtr;

namespace mozilla {

struct CSSPropertyId;
class ErrorResult;
template <typename T>
class MovingNotNull;
struct StyleUnitValue;

namespace dom {

class GlobalObject;

class CSSUnitValue final : public CSSNumericValue {
 public:
  CSSUnitValue(nsCOMPtr<nsISupports> aParent,
               MovingNotNull<UniquePtr<StyleNumericType>> aNumericType,
               double aValue, const nsACString& aUnit);

  static RefPtr<CSSUnitValue> Create(nsCOMPtr<nsISupports> aParent,
                                     const StyleNumericType& aNumericType,
                                     double aValue, const nsACString& aUnit);

  static RefPtr<CSSUnitValue> Create(nsCOMPtr<nsISupports> aParent,
                                     double aValue);

  static RefPtr<CSSUnitValue> Create(nsCOMPtr<nsISupports> aParent,
                                     const StyleUnitValue& aUnitValue);

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;


  static already_AddRefed<CSSUnitValue> Constructor(const GlobalObject& aGlobal,
                                                    double aValue,
                                                    const nsACString& aUnit,
                                                    ErrorResult& aRv);

  double Value() const;

  void SetValue(double aArg);

  void GetUnit(nsCString& aRetVal) const;


  void ToCssTextWithProperty(const CSSPropertyId& aPropertyId,
                             nsACString& aDest) const;

  StyleUnitValue ToStyleUnitValue() const;

 private:
  virtual ~CSSUnitValue() = default;

  double mValue;
  const nsCString mUnit;
};

already_AddRefed<CSSUnitValue> MakeCSSUnitValue(
    nsCOMPtr<nsISupports> aParent, const StyleNumericType& aNumericType,
    double aValue, const nsACString& aUnit);

}  
}  

#endif  // LAYOUT_STYLE_TYPEDOM_CSSUNITVALUE_H_
