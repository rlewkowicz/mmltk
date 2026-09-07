/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_layout_generic_Visibility_h
#define mozilla_layout_generic_Visibility_h

namespace mozilla {

enum class Visibility : uint8_t {
  Untracked,

  ApproximatelyNonVisible,

  ApproximatelyVisible,
};

enum class OnNonvisible : uint8_t {
  DiscardImages  
};

}  

#endif  // mozilla_layout_generic_Visibility_h
