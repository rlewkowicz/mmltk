// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 2005-2012, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  ucasemap.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2005may06
*   created by: Markus W. Scherer
*
*   Case mapping service object and functions using it.
*/

#ifndef __UCASEMAP_H__
#define __UCASEMAP_H__

#include "unicode/utypes.h"
#include "unicode/stringoptions.h"
#include "unicode/ustring.h"

#if U_SHOW_CPLUSPLUS_API
#include "unicode/localpointer.h"
#endif   // U_SHOW_CPLUSPLUS_API


struct UCaseMap;
typedef struct UCaseMap UCaseMap; 

U_CAPI UCaseMap * U_EXPORT2
ucasemap_open(const char *locale, uint32_t options, UErrorCode *pErrorCode);

U_CAPI void U_EXPORT2
ucasemap_close(UCaseMap *csm);

#if U_SHOW_CPLUSPLUS_API

U_NAMESPACE_BEGIN

U_DEFINE_LOCAL_OPEN_POINTER(LocalUCaseMapPointer, UCaseMap, ucasemap_close);

U_NAMESPACE_END

#endif

U_CAPI const char * U_EXPORT2
ucasemap_getLocale(const UCaseMap *csm);

U_CAPI uint32_t U_EXPORT2
ucasemap_getOptions(const UCaseMap *csm);

U_CAPI void U_EXPORT2
ucasemap_setLocale(UCaseMap *csm, const char *locale, UErrorCode *pErrorCode);

U_CAPI void U_EXPORT2
ucasemap_setOptions(UCaseMap *csm, uint32_t options, UErrorCode *pErrorCode);

#if !UCONFIG_NO_BREAK_ITERATION

/**
 * Get the break iterator that is used for titlecasing.
 * Do not modify the returned break iterator.
 * @param csm UCaseMap service object.
 * @return titlecasing break iterator
 * @stable ICU 3.8
 */
U_CAPI const UBreakIterator * U_EXPORT2
ucasemap_getBreakIterator(const UCaseMap *csm);

U_CAPI void U_EXPORT2
ucasemap_setBreakIterator(UCaseMap *csm, UBreakIterator *iterToAdopt, UErrorCode *pErrorCode);

U_CAPI int32_t U_EXPORT2
ucasemap_toTitle(UCaseMap *csm,
                 UChar *dest, int32_t destCapacity,
                 const UChar *src, int32_t srcLength,
                 UErrorCode *pErrorCode);

#endif  // UCONFIG_NO_BREAK_ITERATION

U_CAPI int32_t U_EXPORT2
ucasemap_utf8ToLower(const UCaseMap *csm,
                     char *dest, int32_t destCapacity,
                     const char *src, int32_t srcLength,
                     UErrorCode *pErrorCode);

U_CAPI int32_t U_EXPORT2
ucasemap_utf8ToUpper(const UCaseMap *csm,
                     char *dest, int32_t destCapacity,
                     const char *src, int32_t srcLength,
                     UErrorCode *pErrorCode);

#if !UCONFIG_NO_BREAK_ITERATION

U_CAPI int32_t U_EXPORT2
ucasemap_utf8ToTitle(UCaseMap *csm,
                    char *dest, int32_t destCapacity,
                    const char *src, int32_t srcLength,
                    UErrorCode *pErrorCode);

#endif

U_CAPI int32_t U_EXPORT2
ucasemap_utf8FoldCase(const UCaseMap *csm,
                      char *dest, int32_t destCapacity,
                      const char *src, int32_t srcLength,
                      UErrorCode *pErrorCode);

#endif
