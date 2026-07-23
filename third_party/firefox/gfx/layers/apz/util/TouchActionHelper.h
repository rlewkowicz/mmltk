/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _mozilla_layers_TouchActionHelper_h_
#define _mozilla_layers_TouchActionHelper_h_

#include "mozilla/layers/LayersTypes.h"  // for TouchBehaviorFlags
#include "RelativeTo.h"                  // for RelativeTo

class nsIWidget;
namespace mozilla {

namespace dom {
class Document;
}  

class WidgetTouchEvent;
}  

namespace mozilla::layers {

class TouchActionHelper {
 public:
  static nsTArray<TouchBehaviorFlags> GetAllowedTouchBehavior(
      nsIWidget* aWidget, dom::Document* aDocument,
      const WidgetTouchEvent& aPoint);

  static TouchBehaviorFlags GetAllowedTouchBehaviorForFrame(nsIFrame* aFrame);
};

}  

#endif /*_mozilla_layers_TouchActionHelper_h_ */
