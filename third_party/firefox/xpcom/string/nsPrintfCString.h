/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsPrintfCString_h_
#define nsPrintfCString_h_

#include "nsString.h"

class nsPrintfCString : public nsAutoCStringN<16> {
  typedef nsCString string_type;

 public:
  explicit nsPrintfCString(const char_type* aFormat, ...)
      MOZ_FORMAT_PRINTF(2, 3) {
    va_list ap;
    va_start(ap, aFormat);
    AppendVprintf(aFormat, ap);
    va_end(ap);
  }
};

template <>
struct fmt::formatter<nsPrintfCString, char> : fmt::formatter<nsCString, char> {
};


class nsVprintfCString : public nsAutoCStringN<16> {
  typedef nsCString string_type;

 public:
  nsVprintfCString(const char_type* aFormat, va_list aArgs)
      MOZ_FORMAT_PRINTF(2, 0) {
    AppendVprintf(aFormat, aArgs);
  }
};

template <>
struct fmt::formatter<nsVprintfCString, char>
    : fmt::formatter<nsCString, char> {};

#endif  // !defined(nsPrintfCString_h_)
