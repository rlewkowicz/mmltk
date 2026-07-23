/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef LAYOUT_SVG_SVGVIEWPORTFRAME_H_
#define LAYOUT_SVG_SVGVIEWPORTFRAME_H_

#include "mozilla/ISVGSVGFrame.h"
#include "mozilla/SVGContainerFrame.h"

class gfxContext;

namespace mozilla {

class SVGViewportFrame : public SVGDisplayContainerFrame, public ISVGSVGFrame {
 protected:
  SVGViewportFrame(ComputedStyle* aStyle, nsPresContext* aPresContext,
                   nsIFrame::ClassID aID)
      : SVGDisplayContainerFrame(aStyle, aPresContext, aID) {}

 public:
  NS_DECL_ABSTRACT_FRAME(SVGViewportFrame)

  nsresult AttributeChanged(int32_t aNameSpaceID, nsAtom* aAttribute,
                            AttrModType aModType) override;

  void PaintSVG(gfxContext& aContext, const gfxMatrix& aTransform,
                imgDrawingParams& aImgParams) override;
  void ReflowSVG() override;
  void NotifySVGChanged(ChangeFlags aFlags) override;
  SVGBBox GetBBoxContribution(const Matrix& aToBBoxUserspace,
                              SVGBBoxFlags aFlags) override;
  nsIFrame* GetFrameForPoint(const gfxPoint& aPoint) override;

  bool HasChildrenOnlyTransform(Matrix* aTransform) const override;

  void NotifyViewportOrTransformChanged(ChangeFlags aFlags) override;
};

}  

#endif  // LAYOUT_SVG_SVGVIEWPORTFRAME_H_
