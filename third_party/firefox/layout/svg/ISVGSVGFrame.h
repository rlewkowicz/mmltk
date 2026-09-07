/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef LAYOUT_SVG_ISVGSVGFRAME_H_
#define LAYOUT_SVG_ISVGSVGFRAME_H_

#include "mozilla/ISVGDisplayableFrame.h"
#include "nsQueryFrame.h"

namespace mozilla {

class ISVGSVGFrame {
 public:
  NS_DECL_QUERYFRAME_TARGET(ISVGSVGFrame)

  virtual void NotifyViewportOrTransformChanged(
      ISVGDisplayableFrame::ChangeFlags aFlags) = 0;
};

}  

#endif  // LAYOUT_SVG_ISVGSVGFRAME_H_
