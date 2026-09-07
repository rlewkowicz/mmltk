// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
*   Copyright (C) 2000-2004, International Business Machines
*   Corporation and others.  All Rights Reserved.
**********************************************************************
 *  ucnv_cb.h:
 *  External APIs for the ICU's codeset conversion library
 *  Helena Shih
 * 
 * Modification History:
 *
 *   Date        Name        Description
 */


#ifndef UCNV_CB_H
#define UCNV_CB_H

#include "unicode/utypes.h"

#if !UCONFIG_NO_CONVERSION

#include "unicode/ucnv.h"
#include "unicode/ucnv_err.h"

U_CAPI void U_EXPORT2
ucnv_cbFromUWriteBytes (UConverterFromUnicodeArgs *args,
                        const char* source,
                        int32_t length,
                        int32_t offsetIndex,
                        UErrorCode * err);

U_CAPI void U_EXPORT2 
ucnv_cbFromUWriteSub (UConverterFromUnicodeArgs *args,
                      int32_t offsetIndex,
                      UErrorCode * err);

U_CAPI void U_EXPORT2 ucnv_cbFromUWriteUChars(UConverterFromUnicodeArgs *args,
                             const UChar** source,
                             const UChar*  sourceLimit,
                             int32_t offsetIndex,
                             UErrorCode * err);

U_CAPI void U_EXPORT2 ucnv_cbToUWriteUChars (UConverterToUnicodeArgs *args,
                                             const UChar* source,
                                             int32_t length,
                                             int32_t offsetIndex,
                                             UErrorCode * err);

U_CAPI void U_EXPORT2 ucnv_cbToUWriteSub (UConverterToUnicodeArgs *args,
                       int32_t offsetIndex,
                       UErrorCode * err);
#endif

#endif
