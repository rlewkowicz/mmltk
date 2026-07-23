// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
* Copyright (c) 2002-2014, International Business Machines
* Corporation and others.  All Rights Reserved.
**********************************************************************
* Author: Alan Liu
* Created: November 11 2002
* Since: ICU 2.4
**********************************************************************
*/
#ifndef _USTRENUM_H_
#define _USTRENUM_H_

#include "unicode/uenum.h"
#include "unicode/strenum.h"

U_NAMESPACE_BEGIN

class U_COMMON_API UStringEnumeration : public StringEnumeration {

public:
    UStringEnumeration(UEnumeration* uenum);

    virtual ~UStringEnumeration();

    virtual int32_t count(UErrorCode& status) const override;

    virtual const char* next(int32_t *resultLength, UErrorCode& status) override;

    virtual const UnicodeString* snext(UErrorCode& status) override;

    virtual void reset(UErrorCode& status) override;

    virtual UClassID getDynamicClassID() const override;

    static UClassID U_EXPORT2 getStaticClassID();

    static UStringEnumeration * U_EXPORT2 fromUEnumeration(
            UEnumeration *enumToAdopt, UErrorCode &status);
private:
    UEnumeration *uenum; 
};

U_NAMESPACE_END

#endif

