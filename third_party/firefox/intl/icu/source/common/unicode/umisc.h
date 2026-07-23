// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
*   Copyright (C) 1999-2006, International Business Machines
*   Corporation and others.  All Rights Reserved.
**********************************************************************
*   file name:  umisc.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 1999oct15
*   created by: Markus W. Scherer
*/

#ifndef UMISC_H
#define UMISC_H

#include "unicode/utypes.h"


U_CDECL_BEGIN

typedef struct UFieldPosition {
  int32_t field;
  int32_t beginIndex;
  int32_t endIndex;
} UFieldPosition;

#if !UCONFIG_NO_SERVICE
typedef const void* URegistryKey;
#endif

U_CDECL_END

#endif
