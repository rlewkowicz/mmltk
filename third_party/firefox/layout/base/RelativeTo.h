/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_RelativeTo_h
#define mozilla_RelativeTo_h

#include <ostream>

class nsIFrame;

namespace mozilla {

enum class ViewportType { Layout, Visual };

struct RelativeTo {
  const nsIFrame* mFrame = nullptr;
  ViewportType mViewportType = ViewportType::Layout;
  bool operator==(const RelativeTo&) const = default;
  friend std::ostream& operator<<(std::ostream& aOs, const RelativeTo& aR) {
    return aOs << "{" << aR.mFrame << ", "
               << (aR.mViewportType == ViewportType::Visual ? "visual"
                                                            : "layout")
               << "}";
  }
};

}  

#endif  // mozilla_RelativeTo_h
