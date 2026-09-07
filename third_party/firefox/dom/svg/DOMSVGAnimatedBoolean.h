/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_SVG_DOMSVGANIMATEDBOOLEAN_H_
#define DOM_SVG_DOMSVGANIMATEDBOOLEAN_H_

#include "mozilla/dom/SVGElement.h"
#include "nsWrapperCache.h"

namespace mozilla {
class SVGAnimatedBoolean;
namespace dom {

class DOMSVGAnimatedBoolean final : public nsWrapperCache {
  NS_INLINE_DECL_CYCLE_COLLECTING_NATIVE_REFCOUNTING(DOMSVGAnimatedBoolean)
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_NATIVE_CLASS(DOMSVGAnimatedBoolean)

  DOMSVGAnimatedBoolean(SVGAnimatedBoolean* aVal, SVGElement* aSVGElement)
      : mVal(aVal), mSVGElement(aSVGElement) {}

  SVGElement* GetParentObject() const { return mSVGElement; }
  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;
  bool BaseVal() const;
  void SetBaseVal(bool aValue);
  bool AnimVal() const;

 protected:
  ~DOMSVGAnimatedBoolean();

  SVGAnimatedBoolean* mVal;  
  RefPtr<SVGElement> mSVGElement;
};

}  
}  

#endif  // DOM_SVG_DOMSVGANIMATEDBOOLEAN_H_
