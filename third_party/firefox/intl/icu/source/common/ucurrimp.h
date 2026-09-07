// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
* Copyright (c) 2002-2016, International Business Machines
* Corporation and others.  All Rights Reserved.
**********************************************************************
*/

#ifndef _UCURR_IMP_H_
#define _UCURR_IMP_H_

#include "unicode/utypes.h"
#include "unicode/unistr.h"
#include "unicode/parsepos.h"
#include "unicode/uniset.h"

U_CAPI void
uprv_getStaticCurrencyName(const UChar* iso, const char* loc,
                           icu::UnicodeString& result, UErrorCode& ec);

U_CAPI void
uprv_parseCurrency(const char* locale,
                   const icu::UnicodeString& text,
                   icu::ParsePosition& pos,
                   int8_t type,
                   int32_t* partialMatchLen,
                   UChar* result,
                   UErrorCode& ec);

void uprv_currencyLeads(const char* locale, icu::UnicodeSet& result, UErrorCode& ec);



#endif /* #ifndef _UCURR_IMP_H_ */

