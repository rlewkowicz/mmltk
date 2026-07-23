// License & terms of use: http://www.unicode.org/copyright.html
/*
 *******************************************************************************
 * Copyright (C) 2009-2017, International Business Machines Corporation,       *
 * Google, and others. All Rights Reserved.                                    *
 *******************************************************************************
 */

#ifndef __NOUNIT_H__
#define __NOUNIT_H__

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API

#if !UCONFIG_NO_FORMATTING

#include "unicode/measunit.h"


U_NAMESPACE_BEGIN

namespace NoUnit {
    static inline MeasureUnit U_EXPORT2 base() {
        return {};
    }

    static inline MeasureUnit U_EXPORT2 percent() {
        return MeasureUnit::getPercent();
    }

    static inline MeasureUnit U_EXPORT2 permille() {
        return MeasureUnit::getPermille();
    }
}

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */

#endif /* U_SHOW_CPLUSPLUS_API */

#endif // __NOUNIT_H__
