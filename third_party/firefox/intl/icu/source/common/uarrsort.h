// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 2003-2013, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  uarrsort.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2003aug04
*   created by: Markus W. Scherer
*
*   Internal function for sorting arrays.
*/

#ifndef __UARRSORT_H__
#define __UARRSORT_H__

#include "unicode/utypes.h"

U_CDECL_BEGIN
typedef int32_t U_CALLCONV
UComparator(const void *context, const void *left, const void *right);
U_CDECL_END

U_CAPI void U_EXPORT2
uprv_sortArray(void *array, int32_t length, int32_t itemSize,
               UComparator *cmp, const void *context,
               UBool sortStable, UErrorCode *pErrorCode);

U_CAPI int32_t U_EXPORT2
uprv_uint16Comparator(const void *context, const void *left, const void *right);

U_CAPI int32_t U_EXPORT2
uprv_int32Comparator(const void *context, const void *left, const void *right);

U_CAPI int32_t U_EXPORT2
uprv_uint32Comparator(const void *context, const void *left, const void *right);

U_CAPI int32_t U_EXPORT2
uprv_stableBinarySearch(char *array, int32_t length, void *item, int32_t itemSize,
                        UComparator *cmp, const void *context);

#endif
