/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_SVG_SVGANIMATEDRECT_H_
#define DOM_SVG_SVGANIMATEDRECT_H_

#include "mozilla/dom/SVGElement.h"
#include "nsCycleCollectionParticipant.h"
#include "nsWrapperCache.h"

namespace mozilla {

class SVGAnimatedViewBox;

namespace dom {

class SVGRect;


class SVGAnimatedRect final : public nsWrapperCache {
 public:
  NS_INLINE_DECL_CYCLE_COLLECTING_NATIVE_REFCOUNTING(SVGAnimatedRect)
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_NATIVE_CLASS(SVGAnimatedRect)

  SVGAnimatedRect(SVGAnimatedViewBox* aVal, SVGElement* aSVGElement);

  SVGElement* GetParentObject() const { return mSVGElement; }

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  already_AddRefed<SVGRect> BaseVal();

  already_AddRefed<SVGRect> AnimVal();

 private:
  virtual ~SVGAnimatedRect();

  SVGAnimatedViewBox* mVal;  
  RefPtr<SVGElement> mSVGElement;
};

}  
}  

#endif  // DOM_SVG_SVGANIMATEDRECT_H_
