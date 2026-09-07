// License & terms of use: http://www.unicode.org/copyright.html
/*
********************************************************************************
*   Copyright (C) 1997-2016, International Business Machines
*   Corporation and others.  All Rights Reserved.
********************************************************************************
*
* File DCFMTSYM.H
*
* Modification History:
*
*   Date        Name        Description
*   02/19/97    aliu        Converted from java.
*   03/18/97    clhuang     Updated per C++ implementation.
*   03/27/97    helena      Updated to pass the simple test after code review.
*   08/26/97    aliu        Added currency/intl currency symbol support.
*   07/22/98    stephen     Changed to match C++ style
*                            currencySymbol -> fCurrencySymbol
*                            Constants changed from CAPS to kCaps
*   06/24/99    helena      Integrated Alan's NF enhancements and Java2 bug fixes
*   09/22/00    grhoten     Marked deprecation tags with a pointer to replacement
*                            functions.
********************************************************************************
*/

#ifndef DCFMTSYM_H
#define DCFMTSYM_H

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API

#if !UCONFIG_NO_FORMATTING

#include "unicode/uchar.h"
#include "unicode/uobject.h"
#include "unicode/locid.h"
#include "unicode/numsys.h"
#include "unicode/unum.h"
#include "unicode/unistr.h"



U_NAMESPACE_BEGIN

class U_I18N_API_CLASS DecimalFormatSymbols : public UObject {
public:
    enum ENumberFormatSymbol {
        kDecimalSeparatorSymbol,
        kGroupingSeparatorSymbol,
        kPatternSeparatorSymbol,
        kPercentSymbol,
        kZeroDigitSymbol,
        kDigitSymbol,
        kMinusSignSymbol,
        kPlusSignSymbol,
        kCurrencySymbol,
        kIntlCurrencySymbol,
        kMonetarySeparatorSymbol,
        kExponentialSymbol,
        kPerMillSymbol,
        kPadEscapeSymbol,
        kInfinitySymbol,
        kNaNSymbol,
        kSignificantDigitSymbol,
        kMonetaryGroupingSeparatorSymbol,
        kOneDigitSymbol,
        kTwoDigitSymbol,
        kThreeDigitSymbol,
        kFourDigitSymbol,
        kFiveDigitSymbol,
        kSixDigitSymbol,
        kSevenDigitSymbol,
        kEightDigitSymbol,
        kNineDigitSymbol,
        kExponentMultiplicationSymbol,
#ifndef U_HIDE_INTERNAL_API
        kApproximatelySignSymbol,
#endif  /* U_HIDE_INTERNAL_API */
        kFormatSymbolCount = kExponentMultiplicationSymbol + 2
    };

    U_I18N_API DecimalFormatSymbols(const Locale& locale, UErrorCode& status);

    U_I18N_API DecimalFormatSymbols(const Locale& locale, const NumberingSystem& ns, UErrorCode& status);

    U_I18N_API DecimalFormatSymbols(UErrorCode& status);

    U_I18N_API static DecimalFormatSymbols* createWithLastResortData(UErrorCode& status);

    U_I18N_API DecimalFormatSymbols(const DecimalFormatSymbols&);

    U_I18N_API DecimalFormatSymbols& operator=(const DecimalFormatSymbols&);

    U_I18N_API virtual ~DecimalFormatSymbols();

    U_I18N_API bool operator==(const DecimalFormatSymbols& other) const;

    U_I18N_API bool operator!=(const DecimalFormatSymbols& other) const { return !operator==(other); }

    U_I18N_API inline UnicodeString getSymbol(ENumberFormatSymbol symbol) const;

    U_I18N_API void setSymbol(ENumberFormatSymbol symbol,
                              const UnicodeString& value,
                              const UBool propagateDigits);

#ifndef U_HIDE_INTERNAL_API
    U_I18N_API void setCurrency(const char16_t* currency, UErrorCode& status);
#endif  // U_HIDE_INTERNAL_API

    U_I18N_API inline Locale getLocale() const;

    U_I18N_API Locale getLocale(ULocDataLocaleType type, UErrorCode& status) const;

    U_I18N_API const UnicodeString& getPatternForCurrencySpacing(UCurrencySpacing type,
                                                                 UBool beforeCurrency,
                                                                 UErrorCode& status) const;

    U_I18N_API void setPatternForCurrencySpacing(UCurrencySpacing type,
                                                 UBool beforeCurrency,
                                                 const UnicodeString& pattern);

    U_I18N_API virtual UClassID getDynamicClassID() const override;

    U_I18N_API static UClassID getStaticClassID();

private:
    DecimalFormatSymbols();

    void initialize(const Locale& locale, UErrorCode& success,
                    UBool useLastResortData = false, const NumberingSystem* ns = nullptr);

    void initialize();

public:

#ifndef U_HIDE_INTERNAL_API
    U_I18N_API inline UBool isCustomCurrencySymbol() const {
        return fIsCustomCurrencySymbol;
    }

    U_I18N_API inline UBool isCustomIntlCurrencySymbol() const {
        return fIsCustomIntlCurrencySymbol;
    }

    U_I18N_API inline UChar32 getCodePointZero() const {
        return fCodePointZero;
    }
#endif  /* U_HIDE_INTERNAL_API */

    U_I18N_API inline const UnicodeString& getConstSymbol(ENumberFormatSymbol symbol) const;

#ifndef U_HIDE_INTERNAL_API
    U_I18N_API inline const UnicodeString& getConstDigitSymbol(int32_t digit) const;

    U_I18N_API inline const char16_t* getCurrencyPattern() const;

    U_I18N_API inline const char* getNumberingSystemName() const;
#endif  /* U_HIDE_INTERNAL_API */

private:
    UnicodeString fSymbols[kFormatSymbolCount];

    UnicodeString fNoSymbol;

    UChar32 fCodePointZero;

    Locale locale;

    Locale actualLocale;
    Locale validLocale;
    const char16_t* currPattern = nullptr;

    UnicodeString currencySpcBeforeSym[UNUM_CURRENCY_SPACING_COUNT];
    UnicodeString currencySpcAfterSym[UNUM_CURRENCY_SPACING_COUNT];
    UBool fIsCustomCurrencySymbol;
    UBool fIsCustomIntlCurrencySymbol;
    char nsName[kInternalNumSysNameCapacity+1] = {};
};


inline UnicodeString
DecimalFormatSymbols::getSymbol(ENumberFormatSymbol symbol) const {
    const UnicodeString *strPtr;
    if(symbol < kFormatSymbolCount) {
        strPtr = &fSymbols[symbol];
    } else {
        strPtr = &fNoSymbol;
    }
    return *strPtr;
}

inline const UnicodeString &
DecimalFormatSymbols::getConstSymbol(ENumberFormatSymbol symbol) const {
    const UnicodeString *strPtr;
    if(symbol < kFormatSymbolCount) {
        strPtr = &fSymbols[symbol];
    } else {
        strPtr = &fNoSymbol;
    }
    return *strPtr;
}

#ifndef U_HIDE_INTERNAL_API
inline const UnicodeString& DecimalFormatSymbols::getConstDigitSymbol(int32_t digit) const {
    if (digit < 0 || digit > 9) {
        digit = 0;
    }
    if (digit == 0) {
        return fSymbols[kZeroDigitSymbol];
    }
    ENumberFormatSymbol key = static_cast<ENumberFormatSymbol>(kOneDigitSymbol + digit - 1);
    return fSymbols[key];
}
#endif /* U_HIDE_INTERNAL_API */


inline void
DecimalFormatSymbols::setSymbol(ENumberFormatSymbol symbol, const UnicodeString &value, const UBool propagateDigits = true) {
    if (symbol == kCurrencySymbol) {
        fIsCustomCurrencySymbol = true;
    }
    else if (symbol == kIntlCurrencySymbol) {
        fIsCustomIntlCurrencySymbol = true;
    }
    if(symbol<kFormatSymbolCount) {
        fSymbols[symbol]=value;
    }

    if (symbol == kZeroDigitSymbol) {
        UChar32 sym = value.char32At(0);
        if ( propagateDigits && u_charDigitValue(sym) == 0 && value.countChar32() == 1 ) {
            fCodePointZero = sym;
            for ( int8_t i = 1 ; i<= 9 ; i++ ) {
                sym++;
                fSymbols[static_cast<int>(kOneDigitSymbol) + i - 1] = UnicodeString(sym);
            }
        } else {
            fCodePointZero = -1;
        }
    } else if (symbol >= kOneDigitSymbol && symbol <= kNineDigitSymbol) {
        fCodePointZero = -1;
    }
}


inline Locale
DecimalFormatSymbols::getLocale() const {
    return locale;
}

#ifndef U_HIDE_INTERNAL_API
inline const char16_t*
DecimalFormatSymbols::getCurrencyPattern() const {
    return currPattern;
}
inline const char*
DecimalFormatSymbols::getNumberingSystemName() const {
    return nsName;
}
#endif /* U_HIDE_INTERNAL_API */

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */

#endif /* U_SHOW_CPLUSPLUS_API */

#endif // _DCFMTSYM
