// License & terms of use: http://www.unicode.org/copyright.html
/*  
**********************************************************************
*   Copyright (C) 2002-2016, International Business Machines
*   Corporation and others.  All Rights Reserved.
**********************************************************************
*   file name:  uconfig.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2002sep19
*   created by: Markus W. Scherer
*/

#if !defined(__UCONFIG_H__)
#define __UCONFIG_H__


/*!
 * \file
 * \brief User-configurable settings
 *
 * Miscellaneous switches:
 *
 * A number of macros affect a variety of minor aspects of ICU.
 * Most of them used to be defined elsewhere (e.g., in utypes.h or platform.h)
 * and moved here to make them easier to find.
 *
 * Switches for excluding parts of ICU library code modules:
 *
 * Changing these macros allows building partial, smaller libraries for special purposes.
 * By default, all modules are built.
 * The switches are fairly coarse, controlling large modules.
 * Basic services cannot be turned off.
 *
 * Building with any of these options does not guarantee that the
 * ICU build process will completely work. It is recommended that
 * the ICU libraries and data be built using the normal build.
 * At that time you should remove the data used by those services.
 * After building the ICU data library, you should rebuild the ICU
 * libraries with these switches customized to your needs.
 *
 * @stable ICU 2.4
 */

#if defined(UCONFIG_USE_LOCAL)
#include "uconfig_local.h"
#endif

#if defined(U_DEBUG)
#elif defined(_DEBUG)
#   define U_DEBUG 1
#else
#   define U_DEBUG 0
#endif

#if !defined(UCLN_NO_AUTO_CLEANUP)
#define UCLN_NO_AUTO_CLEANUP 1
#endif

#if !defined(U_DISABLE_RENAMING)
#define U_DISABLE_RENAMING 0
#endif

#if defined(U_NO_DEFAULT_INCLUDE_UTF_HEADERS)
#elif defined(U_COMBINED_IMPLEMENTATION) || defined(U_COMMON_IMPLEMENTATION) || defined(U_I18N_IMPLEMENTATION) || \
      defined(U_IO_IMPLEMENTATION) || defined(U_LAYOUT_IMPLEMENTATION) || defined(U_LAYOUTEX_IMPLEMENTATION) || \
      defined(U_TOOLUTIL_IMPLEMENTATION)
#   define U_NO_DEFAULT_INCLUDE_UTF_HEADERS 1
#else
#   define U_NO_DEFAULT_INCLUDE_UTF_HEADERS 0
#endif

#if !defined(U_OVERRIDE_CXX_ALLOCATION)
#define U_OVERRIDE_CXX_ALLOCATION 1
#endif

#if !defined(U_ENABLE_TRACING)
#define U_ENABLE_TRACING 0
#endif

#if !defined(UCONFIG_ENABLE_PLUGINS)
#define UCONFIG_ENABLE_PLUGINS 0
#endif

#if !defined(U_ENABLE_DYLOAD)
#define U_ENABLE_DYLOAD 1
#endif

#if !defined(U_CHECK_DYLOAD)
#define U_CHECK_DYLOAD 1
#endif

#if !defined(U_DEFAULT_SHOW_DRAFT)
#define U_DEFAULT_SHOW_DRAFT 1
#endif


#if defined(U_HAVE_LIB_SUFFIX)
#elif defined(U_LIB_SUFFIX_C_NAME) || defined(U_IN_DOXYGEN)
#   define U_HAVE_LIB_SUFFIX 1
#endif

#if defined(U_LIB_SUFFIX_C_NAME_STRING)
#elif defined(U_LIB_SUFFIX_C_NAME)
#   define CONVERT_TO_STRING(s) #s
#   define U_LIB_SUFFIX_C_NAME_STRING CONVERT_TO_STRING(U_LIB_SUFFIX_C_NAME)
#else
#   define U_LIB_SUFFIX_C_NAME_STRING ""
#endif


#if !defined(UCONFIG_ONLY_COLLATION)
#   define UCONFIG_ONLY_COLLATION 0
#endif

#if UCONFIG_ONLY_COLLATION
#   define UCONFIG_NO_BREAK_ITERATION 1
#   define UCONFIG_NO_IDNA 1

#if UCONFIG_NO_COLLATION
#       error Contradictory collation switches in uconfig.h.
#endif
#   define UCONFIG_NO_FORMATTING 1
#   define UCONFIG_NO_TRANSLITERATION 1
#   define UCONFIG_NO_REGULAR_EXPRESSIONS 1
#endif


#if !defined(UCONFIG_NO_FILE_IO)
#   define UCONFIG_NO_FILE_IO 0
#endif

#if UCONFIG_NO_FILE_IO && defined(U_TIMEZONE_FILES_DIR)
#   error Contradictory file io switches in uconfig.h.
#endif

#if !defined(UCONFIG_NO_CONVERSION)
#   define UCONFIG_NO_CONVERSION 0
#endif

#if UCONFIG_NO_CONVERSION
#   define UCONFIG_NO_LEGACY_CONVERSION 1
#endif

#if !defined(UCONFIG_ONLY_HTML_CONVERSION)
#   define UCONFIG_ONLY_HTML_CONVERSION 0
#endif

#if !defined(UCONFIG_NO_LEGACY_CONVERSION)
#   define UCONFIG_NO_LEGACY_CONVERSION 0
#endif

#if !defined(UCONFIG_NO_NORMALIZATION)
#   define UCONFIG_NO_NORMALIZATION 0
#endif

#if !defined(UCONFIG_USE_ML_PHRASE_BREAKING)
#   define UCONFIG_USE_ML_PHRASE_BREAKING 0
#endif

#if UCONFIG_NO_NORMALIZATION
#   define UCONFIG_NO_BREAK_ITERATION 1
#   define UCONFIG_NO_IDNA 1

#if UCONFIG_ONLY_COLLATION
#       error Contradictory collation switches in uconfig.h.
#endif
#   define UCONFIG_NO_COLLATION 1
#   define UCONFIG_NO_TRANSLITERATION 1
#endif

#if !defined(UCONFIG_NO_BREAK_ITERATION)
#   define UCONFIG_NO_BREAK_ITERATION 0
#endif

#if !defined(UCONFIG_NO_IDNA)
#   define UCONFIG_NO_IDNA 0
#endif

#if !defined(UCONFIG_MSGPAT_DEFAULT_APOSTROPHE_MODE)
#   define UCONFIG_MSGPAT_DEFAULT_APOSTROPHE_MODE UMSGPAT_APOS_DOUBLE_OPTIONAL
#endif

#if !defined(UCONFIG_USE_WINDOWS_LCID_MAPPING_API)
#   define UCONFIG_USE_WINDOWS_LCID_MAPPING_API 1
#endif


#if !defined(UCONFIG_NO_COLLATION)
#   define UCONFIG_NO_COLLATION 0
#endif

#if !defined(UCONFIG_NO_FORMATTING)
#   define UCONFIG_NO_FORMATTING 0
#endif

#if !defined(UCONFIG_NO_MF2)
#   define UCONFIG_NO_MF2 0
#endif

#if !defined(UCONFIG_NO_TRANSLITERATION)
#   define UCONFIG_NO_TRANSLITERATION 0
#endif

#if !defined(UCONFIG_NO_REGULAR_EXPRESSIONS)
#   define UCONFIG_NO_REGULAR_EXPRESSIONS 0
#endif

#if !defined(UCONFIG_NO_SERVICE)
#   define UCONFIG_NO_SERVICE 0
#endif

#if !defined(UCONFIG_HAVE_PARSEALLINPUT)
#   define UCONFIG_HAVE_PARSEALLINPUT 1
#endif

#if !defined(UCONFIG_NO_FILTERED_BREAK_ITERATION)
#   define UCONFIG_NO_FILTERED_BREAK_ITERATION 0
#endif

#endif
