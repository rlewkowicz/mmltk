// License & terms of use: http://www.unicode.org/copyright.html
/*
******************************************************************************
*
*   Copyright (C) 1997-2012, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
******************************************************************************
*
*  FILE NAME : ptypes.h
*
*   Date        Name        Description
*   05/13/98    nos         Creation (content moved here from ptypes.h).
*   03/02/99    stephen     Added AS400 support.
*   03/30/99    stephen     Added Linux support.
*   04/13/99    stephen     Reworked for autoconf.
*   09/18/08    srl         Moved basic types back to ptypes.h from platform.h
******************************************************************************
*/


#ifndef _PTYPES_H
#define _PTYPES_H

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#include <stddef.h>

#include "unicode/platform.h"


#include <stdint.h>

#if !defined(__cplusplus) && !defined(U_IN_DOXYGEN)
#   if U_HAVE_CHAR16_T
#       include <uchar.h>
#   else
        typedef uint16_t char16_t;
#   endif
#endif

#endif /* _PTYPES_H */
