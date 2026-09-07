/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_intl_EncodingToLang_h
#define mozilla_intl_EncodingToLang_h

#include "nsAtom.h"
#include "mozilla/Encoding.h"

namespace mozilla::intl {

class EncodingToLang {
 public:
  static void Initialize();
  static void Shutdown();

  static nsAtom* Lookup(mozilla::NotNull<const mozilla::Encoding*> aEncoding);

 private:
  static nsAtom* sLangs[];
  static const mozilla::NotNull<const mozilla::Encoding*>*
      kEncodingsByRoughFrequency[];
};

};  

#endif  // mozilla_intl_EncodingToLang_h
