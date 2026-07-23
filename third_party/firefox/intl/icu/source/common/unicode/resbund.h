// License & terms of use: http://www.unicode.org/copyright.html
/*
******************************************************************************
*
*   Copyright (C) 1996-2013, International Business Machines Corporation
*   and others.  All Rights Reserved.
*
******************************************************************************
*
* File resbund.h
*
*   CREATED BY
*       Richard Gillam
*
* Modification History:
*
*   Date        Name        Description
*   2/5/97      aliu        Added scanForLocaleInFile.  Added
*                           constructor which attempts to read resource bundle
*                           from a specific file, without searching other files.
*   2/11/97     aliu        Added UErrorCode return values to constructors.  Fixed
*                           infinite loops in scanForFile and scanForLocale.
*                           Modified getRawResourceData to not delete storage
*                           in localeData and resourceData which it doesn't own.
*                           Added Mac compatibility #ifdefs for tellp() and
*                           ios::nocreate.
*   2/18/97     helena      Updated with 100% documentation coverage.
*   3/13/97     aliu        Rewrote to load in entire resource bundle and store
*                           it as a Hashtable of ResourceBundleData objects.
*                           Added state table to govern parsing of files.
*                           Modified to load locale index out of new file
*                           distinct from default.txt.
*   3/25/97     aliu        Modified to support 2-d arrays, needed for timezone
*                           data. Added support for custom file suffixes.  Again,
*                           needed to support timezone data.
*   4/7/97      aliu        Cleaned up.
* 03/02/99      stephen     Removed dependency on FILE*.
* 03/29/99      helena      Merged Bertrand and Stephen's changes.
* 06/11/99      stephen     Removed parsing of .txt files.
*                           Reworked to use new binary format.
*                           Cleaned up.
* 06/14/99      stephen     Removed methods taking a filename suffix.
* 11/09/99      weiv        Added getLocale(), fRealLocale, removed fRealLocaleID
******************************************************************************
*/

#ifndef RESBUND_H
#define RESBUND_H

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API

#include "unicode/uobject.h"
#include "unicode/ures.h"
#include "unicode/unistr.h"
#include "unicode/locid.h"

 
U_NAMESPACE_BEGIN

class U_COMMON_API ResourceBundle : public UObject {
public:
    ResourceBundle(const UnicodeString&    packageName,
                   const Locale&           locale,
                   UErrorCode&              err);

    ResourceBundle(const UnicodeString&    packageName,
                   UErrorCode&              err);

    ResourceBundle(UErrorCode &err);

    ResourceBundle(const char* packageName,
                   const Locale& locale,
                   UErrorCode& err);

    ResourceBundle(const ResourceBundle &original);

    ResourceBundle(UResourceBundle *res,
                   UErrorCode &status);

    ResourceBundle&
      operator=(const ResourceBundle& other);

    virtual ~ResourceBundle();

    ResourceBundle *clone() const;

    int32_t getSize() const;

    UnicodeString
      getString(UErrorCode& status) const;

    const uint8_t*
      getBinary(int32_t& len, UErrorCode& status) const;


    const int32_t*
      getIntVector(int32_t& len, UErrorCode& status) const;

    uint32_t
      getUInt(UErrorCode& status) const;

    int32_t
      getInt(UErrorCode& status) const;

    UBool hasNext() const;

    void resetIterator();

    const char* getKey() const;

    const char* getName() const;

    UResType getType() const;

    ResourceBundle
      getNext(UErrorCode& status);

    UnicodeString
      getNextString(UErrorCode& status);

    UnicodeString
      getNextString(const char ** key,
                    UErrorCode& status);

    ResourceBundle
      get(int32_t index,
          UErrorCode& status) const;

    UnicodeString
      getStringEx(int32_t index,
                  UErrorCode& status) const;

    ResourceBundle
      get(const char* key,
          UErrorCode& status) const;

    UnicodeString
      getStringEx(const char* key,
                  UErrorCode& status) const;

#ifndef U_HIDE_DEPRECATED_API
    const char* getVersionNumber() const;
#endif  /* U_HIDE_DEPRECATED_API */

    void
      getVersion(UVersionInfo versionInfo) const;

#ifndef U_HIDE_DEPRECATED_API
    const Locale& getLocale() const;
#endif  /* U_HIDE_DEPRECATED_API */

    Locale
      getLocale(ULocDataLocaleType type, UErrorCode &status) const;
#ifndef U_HIDE_INTERNAL_API
    ResourceBundle
        getWithFallback(const char* key, UErrorCode& status);
#endif  /* U_HIDE_INTERNAL_API */
    virtual UClassID getDynamicClassID() const override;

    static UClassID U_EXPORT2 getStaticClassID();

private:
    ResourceBundle() = delete; 

    UResourceBundle *fResource;
    void constructForLocale(const UnicodeString& path, const Locale& locale, UErrorCode& error);
    Locale *fLocale;
};

U_NAMESPACE_END

#endif /* U_SHOW_CPLUSPLUS_API */

#endif
