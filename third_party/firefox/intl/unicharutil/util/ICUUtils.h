/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ICUUtils_h_
#define mozilla_ICUUtils_h_

#ifdef MOZILLA_INTERNAL_API

#  include "nsStringFwd.h"
#  include "unicode/unum.h"  // for UNumberFormat
#  include "mozilla/intl/ICUError.h"
#  include "mozilla/AlreadyAddRefed.h"

class nsIContent;
class nsAtom;

class ICUUtils {
 public:
  class LanguageTagIterForContent {
   public:
    explicit LanguageTagIterForContent(nsIContent* aContent)
        : mContent(aContent), mCurrentFallbackIndex(-1) {}

    already_AddRefed<nsAtom> GetNext();

    bool IsAtStart() const { return mCurrentFallbackIndex < 0; }

   private:
    nsIContent* mContent;
    int8_t mCurrentFallbackIndex;
  };

  static bool LocalizeNumber(double aValue,
                             LanguageTagIterForContent& aLangTags,
                             nsAString& aLocalizedValue);

  static double ParseNumber(const nsAString& aValue,
                            LanguageTagIterForContent& aLangTags);

  static void AssignUCharArrayToString(UChar* aICUString, int32_t aLength,
                                       nsAString& aMozString);

  static nsresult ICUErrorToNsResult(const mozilla::intl::ICUError aError);

#  if 0

  static Locale BCP47CodeToLocale(const nsAString& aBCP47Code);

  static void ToMozString(UnicodeString& aICUString, nsAString& aMozString);
  static void ToICUString(nsAString& aMozString, UnicodeString& aICUString);
#  endif
};

#endif /* MOZILLA_INTERNAL_API */

#endif /* mozilla_ICUUtils_h_ */
