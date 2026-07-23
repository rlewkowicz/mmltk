/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef intl_components_LocaleCanonicalizer_h_
#define intl_components_LocaleCanonicalizer_h_

#include "mozilla/intl/ICU4CGlue.h"
#include "mozilla/Vector.h"

namespace mozilla::intl {

constexpr size_t INITIAL_LOCALE_CANONICALIZER_BUFFER_SIZE = 32;

class LocaleCanonicalizer {
 public:
  using Vector =
      mozilla::Vector<char, INITIAL_LOCALE_CANONICALIZER_BUFFER_SIZE>;

  static ICUResult CanonicalizeICULevel1(
      const char* aLocale, LocaleCanonicalizer::Vector& aLocaleOut);
};

}  
#endif
