// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*   Copyright (C) 1997-2011, International Business Machines
*   Corporation and others.  All Rights Reserved.
*******************************************************************************
*   file name:  uelement.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2011jul04
*   created by: Markus W. Scherer
*
*   Common definitions for UHashTable and UVector.
*   UHashTok moved here from uhash.h and renamed UElement.
*   This allows users of UVector to avoid the confusing #include of uhash.h.
*   uhash.h aliases UElement to UHashTok,
*   so that we need not change all of its code and its users.
*/

#ifndef __UELEMENT_H__
#define __UELEMENT_H__

#include "unicode/utypes.h"

U_CDECL_BEGIN

union UElement {
    void*   pointer;
    int32_t integer;
};
typedef union UElement UElement;

typedef UBool U_CALLCONV UElementsAreEqual(const UElement e1, const UElement e2);

typedef int32_t U_CALLCONV UElementComparator(UElement e1, UElement e2);

typedef void U_CALLCONV UElementAssigner(UElement *dst, UElement *src);

U_CDECL_END

U_CAPI UBool U_EXPORT2 
uhash_compareUnicodeString(const UElement key1, const UElement key2);

U_CAPI UBool U_EXPORT2 
uhash_compareCaselessUnicodeString(const UElement key1, const UElement key2);

#endif  /* __UELEMENT_H__ */
