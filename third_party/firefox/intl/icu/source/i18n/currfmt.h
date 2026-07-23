// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
* Copyright (c) 2004-2014, International Business Machines
* Corporation and others.  All Rights Reserved.
**********************************************************************
* Author: Alan Liu
* Created: April 20, 2004
* Since: ICU 3.0
**********************************************************************
*/
#ifndef CURRENCYFORMAT_H
#define CURRENCYFORMAT_H

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING

#include "unicode/measfmt.h"

U_NAMESPACE_BEGIN

class NumberFormat;

class CurrencyFormat : public MeasureFormat {

 public:

    CurrencyFormat(const Locale& locale, UErrorCode& ec);

    CurrencyFormat(const CurrencyFormat& other);

    virtual ~CurrencyFormat();

    virtual CurrencyFormat* clone() const override;


    using MeasureFormat::format;

    virtual UnicodeString& format(const Formattable& obj,
                                  UnicodeString& appendTo,
                                  FieldPosition& pos,
                                  UErrorCode& ec) const override;

    virtual void parseObject(const UnicodeString& source,
                             Formattable& result,
                             ParsePosition& pos) const override;

    virtual UClassID getDynamicClassID() const override;

    static UClassID U_EXPORT2 getStaticClassID();
};

U_NAMESPACE_END

#endif // #if !UCONFIG_NO_FORMATTING
#endif // #ifndef CURRENCYFORMAT_H
