/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef LAYOUT_STYLE_TYPEDOM_CSSVARIABLEREFERENCEVALUE_H_
#define LAYOUT_STYLE_TYPEDOM_CSSVARIABLEREFERENCEVALUE_H_

#include "js/TypeDecls.h"
#include "mozilla/RefPtr.h"
#include "mozilla/dom/CSSUnparsedValueBindingFwd.h"
#include "nsCOMPtr.h"
#include "nsISupports.h"
#include "nsISupportsImpl.h"
#include "nsString.h"
#include "nsWrapperCache.h"

template <class T>
struct already_AddRefed;

namespace mozilla {

class ErrorResult;
struct StyleVariableReferenceValue;

namespace dom {

class GlobalObject;
class CSSUnparsedValue;

class CSSVariableReferenceValue final : public nsISupports,
                                        public nsWrapperCache {
 public:
  CSSVariableReferenceValue(nsCOMPtr<nsISupports> aParent,
                            const nsACString& aVariable,
                            RefPtr<CSSUnparsedValue> aFallback);

  static RefPtr<CSSVariableReferenceValue> Create(
      nsCOMPtr<nsISupports> aParent,
      const StyleVariableReferenceValue& aVariableReferenceValue);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(CSSVariableReferenceValue)

  nsISupports* GetParentObject() const;

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;


  static already_AddRefed<CSSVariableReferenceValue> Constructor(
      const GlobalObject& aGlobal, const nsACString& aVariable,
      CSSUnparsedValue* aFallback, ErrorResult& aRv);

  void GetVariable(nsCString& aRetVal) const;

  void SetVariable(const nsACString& aArg, ErrorResult& aRv);

  CSSUnparsedValue* GetFallback() const;


  const nsACString& GetVariable() const { return mVariable; };

 private:
  virtual ~CSSVariableReferenceValue() = default;

  nsCOMPtr<nsISupports> mParent;

  nsCString mVariable;
  RefPtr<CSSUnparsedValue> mFallback;
};

}  
}  

#endif  // LAYOUT_STYLE_TYPEDOM_CSSVARIABLEREFERENCEVALUE_H_
