// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*   Copyright (C) 2011-2015, International Business Machines
*   Corporation and others.  All Rights Reserved.
*******************************************************************************
*   file name:  uposixdefs.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2011jul25
*   created by: Markus W. Scherer
*
*   Common definitions for implementation files working with POSIX functions.
*   *Important*: #include this file before any other header files!
*/

#if !defined(__UPOSIXDEFS_H__)
#define __UPOSIXDEFS_H__

#if defined(_XOPEN_SOURCE)
#else
#   define _XOPEN_SOURCE 600
#endif

#if !defined(_XOPEN_SOURCE_EXTENDED) && defined(__TOS_MVS__)
#   define _XOPEN_SOURCE_EXTENDED 1
#endif

#if defined(__cplusplus) && (defined(sun) || 0) && !defined (_STDC_C99)
#   define _STDC_C99
#endif



#endif
