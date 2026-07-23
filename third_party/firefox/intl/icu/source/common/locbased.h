// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
* Copyright (c) 2004-2014, International Business Machines
* Corporation and others.  All Rights Reserved.
**********************************************************************
* Author: Alan Liu
* Created: January 16 2004
* Since: ICU 2.8
**********************************************************************
*/
#ifndef LOCBASED_H
#define LOCBASED_H

#include "unicode/locid.h"
#include "unicode/uobject.h"

U_NAMESPACE_BEGIN

class U_COMMON_API LocaleBased : public UMemory {

 public:

    static const Locale& getLocale(
        const Locale& valid, const Locale& actual,
        ULocDataLocaleType type, UErrorCode& status);

    static const char* getLocaleID(
        const Locale& valid, const Locale& actual,
        ULocDataLocaleType type, UErrorCode& status);

};

U_NAMESPACE_END

#endif
