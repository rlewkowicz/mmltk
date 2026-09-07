/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_DimensionRequest_h
#define mozilla_DimensionRequest_h

#include "Units.h"
#include "mozilla/Maybe.h"

class nsIBaseWindow;
class nsIDocShellTreeOwner;

namespace mozilla {

enum class DimensionKind { Inner, Outer };

struct DimensionRequest {
  DimensionKind mDimensionKind;
  Maybe<LayoutDeviceIntCoord> mX;
  Maybe<LayoutDeviceIntCoord> mY;
  Maybe<LayoutDeviceIntCoord> mWidth;
  Maybe<LayoutDeviceIntCoord> mHeight;

  nsresult SupplementFrom(nsIBaseWindow* aSource);

  nsresult ApplyOuterTo(nsIBaseWindow* aTarget);

  nsresult ApplyInnerTo(nsIDocShellTreeOwner* aTarget, bool aAsRootShell);
};

}  

#endif  // mozilla_DimensionRequest_h
