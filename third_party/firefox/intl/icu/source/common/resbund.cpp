// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
*   Copyright (C) 1997-2013, International Business Machines
*   Corporation and others.  All Rights Reserved.
**********************************************************************
*
* File resbund.cpp
*
* Modification History:
*
*   Date        Name        Description
*   02/05/97    aliu        Fixed bug in chopLocale.  Added scanForLocaleInFile
*                           based on code taken from scanForLocale.  Added
*                           constructor which attempts to read resource bundle
*                           from a specific file, without searching other files.
*   02/11/97    aliu        Added UErrorCode return values to constructors. Fixed
*                           infinite loops in scanForFile and scanForLocale.
*                           Modified getRawResourceData to not delete storage in
*                           localeData and resourceData which it doesn't own.
*                           Added Mac compatibility #ifdefs for tellp() and
*                           ios::nocreate.
*   03/04/97    aliu        Modified to use ExpandingDataSink objects instead of
*                           the highly inefficient ostrstream objects.
*   03/13/97    aliu        Rewrote to load in entire resource bundle and store
*                           it as a Hashtable of ResourceBundleData objects.
*                           Added state table to govern parsing of files.
*                           Modified to load locale index out of new file distinct
*                           from default.txt.
*   03/25/97    aliu        Modified to support 2-d arrays, needed for timezone data.
*                           Added support for custom file suffixes.  Again, needed
*                           to support timezone data.  Improved error handling to
*                           detect duplicate tags and subtags.
*   04/07/97    aliu        Fixed bug in getHashtableForLocale().  Fixed handling
*                           of failing UErrorCode values on entry to API methods.
*                           Fixed bugs in getArrayItem() for negative indices.
*   04/29/97    aliu        Update to use new Hashtable deletion protocol.
*   05/06/97    aliu        Flattened kTransitionTable for HP compiler.
*                           Fixed usage of CharString.
* 06/11/99      stephen     Removed parsing of .txt files.
*                           Reworked to use new binary format.
*                           Cleaned up.
* 06/14/99      stephen     Removed methods taking a filename suffix.
* 06/22/99      stephen     Added missing T_FileStream_close in parse()
* 11/09/99      weiv        Added getLocale(), rewritten constructForLocale()
* March 2000    weiv        complete overhaul.
******************************************************************************
*/

#include "unicode/utypes.h"
#include "unicode/resbund.h"

#include "cmemory.h"
#include "mutex.h"
#include "uassert.h"
#include "umutex.h"

#include "uresimp.h"

U_NAMESPACE_BEGIN


UOBJECT_DEFINE_RTTI_IMPLEMENTATION(ResourceBundle)

ResourceBundle::ResourceBundle(UErrorCode &err)
                                :UObject(), fLocale(nullptr)
{
    fResource = ures_open(nullptr, Locale::getDefault().getName(), &err);
}

ResourceBundle::ResourceBundle(const ResourceBundle &other)
                              :UObject(other), fLocale(nullptr)
{
    UErrorCode status = U_ZERO_ERROR;

    if (other.fResource) {
        fResource = ures_copyResb(nullptr, other.fResource, &status);
    } else {
        fResource = nullptr;
    }
}

ResourceBundle::ResourceBundle(UResourceBundle *res, UErrorCode& err)
                               :UObject(), fLocale(nullptr)
{
    if (res) {
        fResource = ures_copyResb(nullptr, res, &err);
    } else {
        fResource = nullptr;
    }
}

ResourceBundle::ResourceBundle(const char* path, const Locale& locale, UErrorCode& err) 
                               :UObject(), fLocale(nullptr)
{
    fResource = ures_open(path, locale.getName(), &err);
}


ResourceBundle& ResourceBundle::operator=(const ResourceBundle& other)
{
    if(this == &other) {
        return *this;
    }
    if (fResource != nullptr) {
        ures_close(fResource);
        fResource = nullptr;
    }
    if (fLocale != nullptr) {
        delete fLocale;
        fLocale = nullptr;
    }
    UErrorCode status = U_ZERO_ERROR;
    if (other.fResource) {
        fResource = ures_copyResb(nullptr, other.fResource, &status);
    } else {
        fResource = nullptr;
    }
    return *this;
}

ResourceBundle::~ResourceBundle()
{
    if (fResource != nullptr) {
        ures_close(fResource);
    }
    delete fLocale;
}

ResourceBundle *
ResourceBundle::clone() const {
    return new ResourceBundle(*this);
}

UnicodeString ResourceBundle::getString(UErrorCode& status) const {
    int32_t len = 0;
    const char16_t *r = ures_getString(fResource, &len, &status);
    return UnicodeString(true, r, len);
}

const uint8_t *ResourceBundle::getBinary(int32_t& len, UErrorCode& status) const {
    return ures_getBinary(fResource, &len, &status);
}

const int32_t *ResourceBundle::getIntVector(int32_t& len, UErrorCode& status) const {
    return ures_getIntVector(fResource, &len, &status);
}

uint32_t ResourceBundle::getUInt(UErrorCode& status) const {
    return ures_getUInt(fResource, &status);
}

int32_t ResourceBundle::getInt(UErrorCode& status) const {
    return ures_getInt(fResource, &status);
}

const char *ResourceBundle::getName() const {
    return ures_getName(fResource);
}

const char *ResourceBundle::getKey() const {
    return ures_getKey(fResource);
}

UResType ResourceBundle::getType() const {
    return ures_getType(fResource);
}

int32_t ResourceBundle::getSize() const {
    return ures_getSize(fResource);
}

UBool ResourceBundle::hasNext() const {
    return ures_hasNext(fResource);
}

void ResourceBundle::resetIterator() {
    ures_resetIterator(fResource);
}

ResourceBundle ResourceBundle::getNext(UErrorCode& status) {
    UResourceBundle r;

    ures_initStackObject(&r);
    ures_getNextResource(fResource, &r, &status);
    ResourceBundle res(&r, status);
    if (U_SUCCESS(status)) {
        ures_close(&r);
    }
    return res;
}

UnicodeString ResourceBundle::getNextString(UErrorCode& status) {
    int32_t len = 0;
    const char16_t* r = ures_getNextString(fResource, &len, nullptr, &status);
    return UnicodeString(true, r, len);
}

UnicodeString ResourceBundle::getNextString(const char ** key, UErrorCode& status) {
    int32_t len = 0;
    const char16_t* r = ures_getNextString(fResource, &len, key, &status);
    return UnicodeString(true, r, len);
}

ResourceBundle ResourceBundle::get(int32_t indexR, UErrorCode& status) const {
    UResourceBundle r;

    ures_initStackObject(&r);
    ures_getByIndex(fResource, indexR, &r, &status);
    ResourceBundle res(&r, status);
    if (U_SUCCESS(status)) {
        ures_close(&r);
    }
    return res;
}

UnicodeString ResourceBundle::getStringEx(int32_t indexS, UErrorCode& status) const {
    int32_t len = 0;
    const char16_t* r = ures_getStringByIndex(fResource, indexS, &len, &status);
    return UnicodeString(true, r, len);
}

ResourceBundle ResourceBundle::get(const char* key, UErrorCode& status) const {
    UResourceBundle r;

    ures_initStackObject(&r);
    ures_getByKey(fResource, key, &r, &status);
    ResourceBundle res(&r, status);
    if (U_SUCCESS(status)) {
        ures_close(&r);
    }
    return res;
}

ResourceBundle ResourceBundle::getWithFallback(const char* key, UErrorCode& status){
    UResourceBundle r;
    ures_initStackObject(&r);
    ures_getByKeyWithFallback(fResource, key, &r, &status);
    ResourceBundle res(&r, status);
    if(U_SUCCESS(status)){
        ures_close(&r);
    }
    return res;
}
UnicodeString ResourceBundle::getStringEx(const char* key, UErrorCode& status) const {
    int32_t len = 0;
    const char16_t* r = ures_getStringByKey(fResource, key, &len, &status);
    return UnicodeString(true, r, len);
}

const char*
ResourceBundle::getVersionNumber()  const
{
    return ures_getVersionNumberInternal(fResource);
}

void ResourceBundle::getVersion(UVersionInfo versionInfo) const {
    ures_getVersion(fResource, versionInfo);
}

const Locale &ResourceBundle::getLocale() const {
    static UMutex gLocaleLock;
    Mutex lock(&gLocaleLock);
    if (fLocale != nullptr) {
        return *fLocale;
    }
    UErrorCode status = U_ZERO_ERROR;
    const char *localeName = ures_getLocaleInternal(fResource, &status);
    ResourceBundle *ncThis = const_cast<ResourceBundle *>(this);
    ncThis->fLocale = new Locale(localeName);
    return ncThis->fLocale != nullptr ? *ncThis->fLocale : Locale::getDefault();
}

Locale ResourceBundle::getLocale(ULocDataLocaleType type, UErrorCode &status) const
{
  return ures_getLocaleByType(fResource, type, &status);
}

U_NAMESPACE_END
