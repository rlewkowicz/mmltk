/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(BUILD_CONSTANTS_H_)
#define BUILD_CONSTANTS_H_


namespace mozilla {

constexpr bool kIsDebug =
#if defined(DEBUG)
    true;
#else
    false;
#endif

constexpr bool kIsWindows =
    false;

constexpr bool kIsMacOS =
    false;

constexpr bool kIsLinux =
#if defined(MOZ_WIDGET_GTK)
    true;
#else
    false;
#endif

constexpr bool kIsAndroid =
    false;

constexpr bool kIsDmd =
#if defined(MOZ_DMD)
    true;
#else
    false;
#endif

}  

#endif
