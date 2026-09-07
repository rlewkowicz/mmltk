// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 2003-2014, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  ucol_swp.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2003sep10
*   created by: Markus W. Scherer
*
*   Swap collation binaries.
*/

#ifndef __UCOL_SWP_H__
#define __UCOL_SWP_H__

#include "unicode/utypes.h"

#if !UCONFIG_NO_COLLATION

#include "udataswp.h"

U_CAPI UBool U_EXPORT2
ucol_looksLikeCollationBinary(const UDataSwapper *ds,
                              const void *inData, int32_t length);

U_CAPI int32_t U_EXPORT2
ucol_swap(const UDataSwapper *ds,
          const void *inData, int32_t length, void *outData,
          UErrorCode *pErrorCode);

U_CAPI int32_t U_EXPORT2
ucol_swapInverseUCA(const UDataSwapper *ds,
                    const void *inData, int32_t length, void *outData,
                    UErrorCode *pErrorCode);

#endif /* #if !UCONFIG_NO_COLLATION */

#endif
