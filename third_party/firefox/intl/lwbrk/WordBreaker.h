/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef mozilla_intl_WordBreaker_h_
#define mozilla_intl_WordBreaker_h_

#include "nsStringFwd.h"
#include <cstdint>

#define NS_WORDBREAKER_NEED_MORE_TEXT -1

namespace mozilla {
namespace intl {

struct WordRange {
  uint32_t mBegin;
  uint32_t mEnd;
};

class WordBreaker final {
 public:
  WordBreaker() = delete;
  ~WordBreaker() = delete;

  enum class FindWordOptions { None, StopAtPunctuation };

  static WordRange FindWord(
      const nsAString& aText, uint32_t aPos,
      const FindWordOptions aOptions = FindWordOptions::None);
};

}  
}  

#endif /* mozilla_intl_WordBreaker_h_ */
