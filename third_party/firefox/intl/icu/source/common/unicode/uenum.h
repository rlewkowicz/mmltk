// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 2002-2013, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  uenum.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:2
*
*   created on: 2002jul08
*   created by: Vladimir Weinstein
*/

#ifndef __UENUM_H
#define __UENUM_H

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API
#include "unicode/localpointer.h"

U_NAMESPACE_BEGIN
class StringEnumeration;
U_NAMESPACE_END
#endif   // U_SHOW_CPLUSPLUS_API

 
struct UEnumeration;
typedef struct UEnumeration UEnumeration;

U_CAPI void U_EXPORT2
uenum_close(UEnumeration* en);

#if U_SHOW_CPLUSPLUS_API

U_NAMESPACE_BEGIN

U_DEFINE_LOCAL_OPEN_POINTER(LocalUEnumerationPointer, UEnumeration, uenum_close);

U_NAMESPACE_END

#endif

U_CAPI int32_t U_EXPORT2
uenum_count(UEnumeration* en, UErrorCode* status);

U_CAPI const UChar* U_EXPORT2
uenum_unext(UEnumeration* en,
            int32_t* resultLength,
            UErrorCode* status);

U_CAPI const char* U_EXPORT2
uenum_next(UEnumeration* en,
           int32_t* resultLength,
           UErrorCode* status);

U_CAPI void U_EXPORT2
uenum_reset(UEnumeration* en, UErrorCode* status);

#if U_SHOW_CPLUSPLUS_API

U_CAPI UEnumeration* U_EXPORT2
uenum_openFromStringEnumeration(icu::StringEnumeration* adopted, UErrorCode* ec);

#endif

U_CAPI UEnumeration* U_EXPORT2
uenum_openUCharStringsEnumeration(const UChar* const strings[], int32_t count,
                                 UErrorCode* ec);

U_CAPI UEnumeration* U_EXPORT2
uenum_openCharStringsEnumeration(const char* const strings[], int32_t count,
                                 UErrorCode* ec);

#endif
