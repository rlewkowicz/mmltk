/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsFmtCString_h_
#define nsFmtCString_h_

#include "fmt/format.h"
#include "fmt/xchar.h"
#include "nsString.h"

template <typename T>
class nsTFmtString : public nsTAutoStringN<T, 16> {
 public:
  template <typename... Args>
  explicit nsTFmtString(
      fmt::basic_format_string<T, std::type_identity_t<Args>...> aFormatStr,
      Args&&... aArgs) {
    this->AppendFmt(aFormatStr, std::forward<Args>(aArgs)...);
  }
};

template <typename Char>
struct fmt::formatter<nsTFmtString<Char>, Char>
    : fmt::formatter<nsTString<Char>, Char> {};

#endif  // !defined(nsFmtString_h___)
