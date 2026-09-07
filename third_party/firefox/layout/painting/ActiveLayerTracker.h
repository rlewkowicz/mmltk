/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ACTIVELAYERTRACKER_H_
#define ACTIVELAYERTRACKER_H_

#include "NonCustomCSSPropertyId.h"

class nsIFrame;
class nsIContent;
class nsCSSPropertyIDSet;
class nsDOMCSSDeclaration;

namespace mozilla {

class nsDisplayListBuilder;

class ActiveLayerTracker {
 public:
  static void Shutdown();


  static void NotifyRestyle(nsIFrame* aFrame, NonCustomCSSPropertyId aProperty);

  static void NotifyInlineStyleRuleModified(nsIFrame* aFrame,
                                            NonCustomCSSPropertyId aProperty);
  static void NotifyNeedsRepaint(nsIFrame* aFrame);

  static bool IsTransformAnimated(nsIFrame* aFrame);

  static bool IsScaleSubjectToAnimation(nsIFrame* aFrame);

  static bool IsOpacityAnimated(nsIFrame* aFrame);

  static void TransferActivityToContent(nsIFrame* aFrame, nsIContent* aContent);
  static void TransferActivityToFrame(nsIContent* aContent, nsIFrame* aFrame);
};

}  

#endif /* ACTIVELAYERTRACKER_H_ */
