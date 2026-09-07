// License & terms of use: http://www.unicode.org/copyright.html
/*  
**********************************************************************
*   Copyright (C) 1999-2010, International Business Machines
*   Corporation and others.  All Rights Reserved.
**********************************************************************
*   file name:  ustr_cnv.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2004Aug27
*   created by: George Rhoten
*/

#ifndef USTR_CNV_IMP_H
#define USTR_CNV_IMP_H

#include "unicode/utypes.h"
#include "unicode/ucnv.h"

#if !UCONFIG_NO_CONVERSION

U_CAPI UConverter* U_EXPORT2
u_getDefaultConverter(UErrorCode *status);


U_CAPI void U_EXPORT2
u_releaseDefaultConverter(UConverter *converter);

U_CAPI void U_EXPORT2
u_flushDefaultConverter(void);

#endif

#endif
