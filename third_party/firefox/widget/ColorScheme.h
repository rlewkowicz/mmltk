/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ColorScheme_h
#define mozilla_ColorScheme_h

#include <cstdint>

namespace mozilla {

enum class ColorScheme : uint8_t { Light, Dark };

enum class ColorSchemeMode : uint8_t {
  Used,
  Preferred,
};

}  

#endif
