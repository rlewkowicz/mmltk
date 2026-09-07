// License & terms of use: http://www.unicode.org/copyright.html
/*
*****************************************************************************************
* Copyright (C) 2015-2016, International Business Machines
* Corporation and others. All Rights Reserved.
*****************************************************************************************
*/

#ifndef UFIELDPOSITER_H
#define UFIELDPOSITER_H

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING

#if U_SHOW_CPLUSPLUS_API
#include "unicode/localpointer.h"
#endif   // U_SHOW_CPLUSPLUS_API


struct UFieldPositionIterator;
typedef struct UFieldPositionIterator UFieldPositionIterator;  

U_CAPI UFieldPositionIterator* U_EXPORT2
ufieldpositer_open(UErrorCode* status);

U_CAPI void U_EXPORT2
ufieldpositer_close(UFieldPositionIterator *fpositer);


#if U_SHOW_CPLUSPLUS_API

U_NAMESPACE_BEGIN

U_DEFINE_LOCAL_OPEN_POINTER(LocalUFieldPositionIteratorPointer, UFieldPositionIterator, ufieldpositer_close);

U_NAMESPACE_END

#endif

U_CAPI int32_t U_EXPORT2
ufieldpositer_next(UFieldPositionIterator *fpositer,
                   int32_t *beginIndex, int32_t *endIndex);

#endif /* #if !UCONFIG_NO_FORMATTING */

#endif
