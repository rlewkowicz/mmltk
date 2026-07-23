// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
* Copyright (c) 2004-2014, International Business Machines
* Corporation and others.  All Rights Reserved.
**********************************************************************
* Author: Alan Liu
* Created: April 26, 2004
* Since: ICU 3.0
**********************************************************************
*/
#ifndef __CURRENCYUNIT_H__
#define __CURRENCYUNIT_H__

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API

#if !UCONFIG_NO_FORMATTING

#include "unicode/measunit.h"

 
U_NAMESPACE_BEGIN

class U_I18N_API CurrencyUnit: public MeasureUnit {
 public:
    CurrencyUnit();

    CurrencyUnit(ConstChar16Ptr isoCode, UErrorCode &ec);

    CurrencyUnit(StringPiece isoCode, UErrorCode &ec);

    CurrencyUnit(const CurrencyUnit& other);

    CurrencyUnit(const MeasureUnit& measureUnit, UErrorCode &ec);

    CurrencyUnit& operator=(const CurrencyUnit& other);

    virtual CurrencyUnit* clone() const override;

    virtual ~CurrencyUnit();

    virtual UClassID getDynamicClassID() const override;

    static UClassID U_EXPORT2 getStaticClassID();

    inline const char16_t* getISOCurrency() const;

 private:
    char16_t isoCode[4];
};

inline const char16_t* CurrencyUnit::getISOCurrency() const {
    return isoCode;
}

U_NAMESPACE_END

#endif // !UCONFIG_NO_FORMATTING

#endif /* U_SHOW_CPLUSPLUS_API */

#endif // __CURRENCYUNIT_H__
