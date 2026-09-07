// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 1998-2014, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*
* Private implementation header for C collation
*   file name:  ucol_imp.h
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2000dec11
*   created by: Vladimir Weinstein
*
* Modification history
* Date        Name      Comments
* 02/16/2001  synwee    Added UCOL_GETPREVCE for the use in ucoleitr
* 02/27/2001  synwee    Added getMaxExpansion data structure in UCollator
* 03/02/2001  synwee    Added UCOL_IMPLICIT_CE
* 03/12/2001  synwee    Added pointer start to collIterate.
*/

#ifndef UCOL_IMP_H
#define UCOL_IMP_H

#include "unicode/utypes.h"

#if !UCONFIG_NO_COLLATION


#include "unicode/ucol.h"

U_CAPI UBool U_EXPORT2
ucol_equals(const UCollator *source, const UCollator *target);

#define U_ICUDATA_COLL U_ICUDATA_NAME U_TREE_SEPARATOR_STRING "coll"

#ifdef __cplusplus

#include "unicode/locid.h"
#include "unicode/ures.h"

U_NAMESPACE_BEGIN

struct CollationCacheEntry;

class Locale;
class UnicodeString;
class UnifiedCache;

class CollationLoader {
public:
    static void appendRootRules(UnicodeString &s);
    static void loadRules(const char *localeID, const char *collationType,
                          UnicodeString &rules, UErrorCode &errorCode);
    static const CollationCacheEntry *loadTailoring(const Locale &locale, UErrorCode &errorCode);

    const CollationCacheEntry *createCacheEntry(UErrorCode &errorCode);

private:
    static void U_CALLCONV loadRootRules(UErrorCode &errorCode);

    static const uint32_t TRIED_SEARCH = 1;
    static const uint32_t TRIED_DEFAULT = 2;
    static const uint32_t TRIED_STANDARD = 4;

    CollationLoader(const CollationCacheEntry *re, const Locale &requested, UErrorCode &errorCode);
    ~CollationLoader();

    const CollationCacheEntry *loadFromLocale(UErrorCode &errorCode);
    const CollationCacheEntry *loadFromBundle(UErrorCode &errorCode);
    const CollationCacheEntry *loadFromCollations(UErrorCode &errorCode);
    const CollationCacheEntry *loadFromData(UErrorCode &errorCode);

    const CollationCacheEntry *getCacheEntry(UErrorCode &errorCode);

    const CollationCacheEntry *makeCacheEntryFromRoot(
            const Locale &loc, UErrorCode &errorCode) const;

    static const CollationCacheEntry *makeCacheEntry(
            const Locale &loc,
            const CollationCacheEntry *entryFromCache,
            UErrorCode &errorCode);

    const UnifiedCache *cache;
    const CollationCacheEntry *rootEntry;
    Locale validLocale;
    Locale locale;
    char type[16];
    char defaultType[16];
    uint32_t typesTried;
    UBool typeFallback;
    UResourceBundle *bundle;
    UResourceBundle *collations;
    UResourceBundle *data;
};

U_NAMESPACE_END

#endif  /* __cplusplus */

#endif /* #if !UCONFIG_NO_COLLATION */

#endif
