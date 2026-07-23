// License & terms of use: http://www.unicode.org/copyright.html
/*
******************************************************************************
*
*   Copyright (C) 1997-2014, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
******************************************************************************
*
*  FILE NAME : putil.h
*
*   Date        Name        Description
*   05/14/98    nos         Creation (content moved here from utypes.h).
*   06/17/99    erm         Added IEEE_754
*   07/22/98    stephen     Added IEEEremainder, max, min, trunc
*   08/13/98    stephen     Added isNegativeInfinity, isPositiveInfinity
*   08/24/98    stephen     Added longBitsFromDouble
*   03/02/99    stephen     Removed openFile().  Added AS400 support.
*   04/15/99    stephen     Converted to C
*   11/15/99    helena      Integrated S/390 changes for IEEE support.
*   01/11/00    helena      Added u_getVersion.
******************************************************************************
*/

#if !defined(PUTIL_H)
#define PUTIL_H

#include "unicode/utypes.h"



U_CAPI const char* U_EXPORT2 u_getDataDirectory(void);


U_CAPI void U_EXPORT2 u_setDataDirectory(const char *directory);

#if !defined(U_HIDE_INTERNAL_API)
U_CAPI const char * U_EXPORT2 u_getTimeZoneFilesDirectory(UErrorCode *status);

U_CAPI void U_EXPORT2 u_setTimeZoneFilesDirectory(const char *path, UErrorCode *status);
#endif


#   define U_FILE_SEP_CHAR '/'
#   define U_FILE_ALT_SEP_CHAR '/'
#   define U_PATH_SEP_CHAR ':'
#   define U_FILE_SEP_STRING "/"
#   define U_FILE_ALT_SEP_STRING "/"
#   define U_PATH_SEP_STRING ":"


U_CAPI void U_EXPORT2
u_charsToUChars(const char *cs, UChar *us, int32_t length);

U_CAPI void U_EXPORT2
u_UCharsToChars(const UChar *us, char *cs, int32_t length);

#endif
