/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ScrollOrigin_h_
#define mozilla_ScrollOrigin_h_

#include "mozilla/DefineEnum.h"

namespace mozilla {

MOZ_DEFINE_ENUM_CLASS_WITH_BASE_AND_TOSTRING(
    ScrollOrigin, uint8_t,
    (
        None,

        NotSpecified,
        Apz,
        Restore,
        Relative,
        Clamp,


        Other,
        Pixels,
        Lines,
        Pages,
        MouseWheel,
        Scrollbars));

}  

#endif  // mozilla_ScrollOrigin_h_
