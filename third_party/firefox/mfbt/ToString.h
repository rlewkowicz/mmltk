/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_ToString_h
#define mozilla_ToString_h

#include <string>
#include <sstream>

namespace mozilla {

template <typename T>
std::string ToString(const T& aValue) {
  std::ostringstream stream;
  stream << aValue;
  return stream.str();
}

}  

#endif /* mozilla_ToString_h */
