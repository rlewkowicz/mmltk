/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/SVGAnimateMotionElement.h"

#include "mozilla/dom/SVGAnimateMotionElementBinding.h"

NS_IMPL_NS_NEW_SVG_ELEMENT(AnimateMotion)

namespace mozilla::dom {

JSObject* SVGAnimateMotionElement::WrapNode(JSContext* aCx,
                                            JS::Handle<JSObject*> aGivenProto) {
  return SVGAnimateMotionElement_Binding::Wrap(aCx, this, aGivenProto);
}


SVGAnimateMotionElement::SVGAnimateMotionElement(
    already_AddRefed<mozilla::dom::NodeInfo> aNodeInfo)
    : SVGAnimationElement(std::move(aNodeInfo)) {}


NS_IMPL_ELEMENT_CLONE_WITH_INIT(SVGAnimateMotionElement)


SMILAnimationFunction& SVGAnimateMotionElement::AnimationFunction() {
  return mAnimationFunction;
}

bool SVGAnimateMotionElement::GetTargetAttributeName(
    int32_t* aNamespaceID, nsAtom** aLocalName) const {
  *aNamespaceID = kNameSpaceID_None;
  *aLocalName = nsGkAtoms::mozAnimateMotionDummyAttr;
  return true;
}

}  
