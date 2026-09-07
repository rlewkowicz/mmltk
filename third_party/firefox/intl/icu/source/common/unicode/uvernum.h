// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*   Copyright (C) 2000-2016, International Business Machines
*   Corporation and others.  All Rights Reserved.
*******************************************************************************
*
*   file name:  uvernum.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   Created by: Vladimir Weinstein
*   Updated by: Steven R. Loomis
*
*/


 /*
  * IMPORTANT: When updating version, the following things need to be done:
  * source/common/unicode/uvernum.h - this file: update major, minor,
  *        patchlevel, suffix, version, short version constants, namespace,
  *                    renaming macro, and copyright
  *
  * The following files need to be updated as well, which can be done
  *  by running the UNIX makefile target 'update-windows-makefiles' in icu4c/source.
  *
  * source/allinone/Build.Windows.IcuVersion.props - Update the IcuMajorVersion
  * source/data/makedata.mak - change U_ICUDATA_NAME so that it contains
  *                            the new major/minor combination, and UNICODE_VERSION
  *                            for the Unicode version.
  */

#ifndef UVERNUM_H
#define UVERNUM_H

/** The standard copyright notice that gets compiled into each library.
 *  This value will change in the subsequent releases of ICU
 *  @stable ICU 2.4
 */
#define U_COPYRIGHT_STRING \
  " Copyright (C) 2016 and later: Unicode, Inc. and others. License & terms of use: http://www.unicode.org/copyright.html "

#define U_ICU_VERSION_MAJOR_NUM 78

#define U_ICU_VERSION_MINOR_NUM 3

#define U_ICU_VERSION_PATCHLEVEL_NUM 0

#ifndef U_ICU_VERSION_BUILDLEVEL_NUM
#define U_ICU_VERSION_BUILDLEVEL_NUM 0
#endif

#define U_ICU_VERSION_SUFFIX _78

#ifndef U_DISABLE_VERSION_SUFFIX
#define U_DISABLE_VERSION_SUFFIX 0
#endif

#ifndef U_ICU_ENTRY_POINT_RENAME
#ifdef U_HAVE_LIB_SUFFIX
#   if !U_DISABLE_VERSION_SUFFIX
#       define U_DEF_ICU_ENTRY_POINT_RENAME(x,y,z) x ## y ##  z
#       define U_DEF2_ICU_ENTRY_POINT_RENAME(x,y,z) U_DEF_ICU_ENTRY_POINT_RENAME(x,y,z)
#       define U_ICU_ENTRY_POINT_RENAME(x)    U_DEF2_ICU_ENTRY_POINT_RENAME(x,U_ICU_VERSION_SUFFIX,U_LIB_SUFFIX_C_NAME)
#   else
#       define U_DEF_ICU_ENTRY_POINT_RENAME(x,y) x ## y
#       define U_DEF2_ICU_ENTRY_POINT_RENAME(x,y) U_DEF_ICU_ENTRY_POINT_RENAME(x,y)
#       define U_ICU_ENTRY_POINT_RENAME(x)    U_DEF2_ICU_ENTRY_POINT_RENAME(x,U_LIB_SUFFIX_C_NAME)
#   endif
#else
#   if !U_DISABLE_VERSION_SUFFIX
#       define U_DEF_ICU_ENTRY_POINT_RENAME(x,y) x ## y
#       define U_DEF2_ICU_ENTRY_POINT_RENAME(x,y) U_DEF_ICU_ENTRY_POINT_RENAME(x,y)
#       define U_ICU_ENTRY_POINT_RENAME(x)    U_DEF2_ICU_ENTRY_POINT_RENAME(x,U_ICU_VERSION_SUFFIX)
#   else
#       define U_ICU_ENTRY_POINT_RENAME(x)    x
#   endif
#endif
#endif

#define U_ICU_VERSION "78.3"

#define U_ICU_VERSION_SHORT "78"

#ifndef U_HIDE_INTERNAL_API
#define U_ICU_DATA_VERSION "78.3"
#endif  /* U_HIDE_INTERNAL_API */


#define UCOL_RUNTIME_VERSION 9

#define UCOL_BUILDER_VERSION 9

#ifndef U_HIDE_DEPRECATED_API
#define UCOL_TAILORINGS_VERSION 1
#endif  /* U_HIDE_DEPRECATED_API */

#endif
