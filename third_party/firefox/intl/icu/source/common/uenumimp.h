// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 2002-2006, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  uenumimp.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:2
*
*   created on: 2002jul08
*   created by: Vladimir Weinstein
*/

#ifndef __UENUMIMP_H
#define __UENUMIMP_H

#include "unicode/uenum.h"

U_CDECL_BEGIN


typedef void U_CALLCONV
UEnumClose(UEnumeration *en);

typedef int32_t U_CALLCONV
UEnumCount(UEnumeration *en, UErrorCode *status);

typedef const UChar* U_CALLCONV 
UEnumUNext(UEnumeration* en,
            int32_t* resultLength,
            UErrorCode* status);

typedef const char* U_CALLCONV 
UEnumNext(UEnumeration* en,
           int32_t* resultLength,
           UErrorCode* status);

typedef void U_CALLCONV 
UEnumReset(UEnumeration* en, 
            UErrorCode* status);


struct UEnumeration {
    void *baseContext;

    void *context;

    UEnumClose *close;
    UEnumCount *count;
    UEnumUNext *uNext;
    UEnumNext  *next;
    UEnumReset *reset;
};

U_CDECL_END

U_CAPI const UChar* U_EXPORT2
uenum_unextDefault(UEnumeration* en,
            int32_t* resultLength,
            UErrorCode* status);

U_CAPI const char* U_EXPORT2
uenum_nextDefault(UEnumeration* en,
            int32_t* resultLength,
            UErrorCode* status);

#endif
