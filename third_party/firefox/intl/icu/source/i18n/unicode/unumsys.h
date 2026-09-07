// License & terms of use: http://www.unicode.org/copyright.html
/*
*****************************************************************************************
* Copyright (C) 2013-2014, International Business Machines
* Corporation and others. All Rights Reserved.
*****************************************************************************************
*/

#ifndef UNUMSYS_H
#define UNUMSYS_H

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING

#include "unicode/uenum.h"

#if U_SHOW_CPLUSPLUS_API
#include "unicode/localpointer.h"
#endif   // U_SHOW_CPLUSPLUS_API


struct UNumberingSystem;
typedef struct UNumberingSystem UNumberingSystem;  

U_CAPI UNumberingSystem * U_EXPORT2
unumsys_open(const char *locale, UErrorCode *status);

U_CAPI UNumberingSystem * U_EXPORT2
unumsys_openByName(const char *name, UErrorCode *status);

U_CAPI void U_EXPORT2
unumsys_close(UNumberingSystem *unumsys);

#if U_SHOW_CPLUSPLUS_API
U_NAMESPACE_BEGIN

U_DEFINE_LOCAL_OPEN_POINTER(LocalUNumberingSystemPointer, UNumberingSystem, unumsys_close);

U_NAMESPACE_END
#endif

U_CAPI UEnumeration * U_EXPORT2
unumsys_openAvailableNames(UErrorCode *status);

U_CAPI const char * U_EXPORT2
unumsys_getName(const UNumberingSystem *unumsys);

U_CAPI UBool U_EXPORT2
unumsys_isAlgorithmic(const UNumberingSystem *unumsys);

U_CAPI int32_t U_EXPORT2
unumsys_getRadix(const UNumberingSystem *unumsys);

U_CAPI int32_t U_EXPORT2
unumsys_getDescription(const UNumberingSystem *unumsys, UChar *result,
                       int32_t resultLength, UErrorCode *status);

#endif /* #if !UCONFIG_NO_FORMATTING */

#endif
