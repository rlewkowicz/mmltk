// License & terms of use: http://www.unicode.org/copyright.html
/*************************************************************************
* Copyright (c) 1997-2016, International Business Machines Corporation
* and others. All Rights Reserved.
**************************************************************************
*
* File TIMEZONE.H
*
* Modification History:
*
*   Date        Name        Description
*   04/21/97    aliu        Overhauled header.
*   07/09/97    helena      Changed createInstance to createDefault.
*   08/06/97    aliu        Removed dependency on internal header for Hashtable.
*   08/10/98    stephen        Changed getDisplayName() API conventions to match
*   08/19/98    stephen        Changed createTimeZone() to never return 0
*   09/02/98    stephen        Sync to JDK 1.2 8/31
*                            - Added getOffset(... monthlen ...)
*                            - Added hasSameRules()
*   09/15/98    stephen        Added getStaticClassID
*   12/03/99    aliu        Moved data out of static table into icudata.dll.
*                           Hashtable replaced by new static data structures.
*   12/14/99    aliu        Made GMT public.
*   08/15/01    grhoten     Made GMT private and added the getGMT() function
**************************************************************************
*/

#ifndef TIMEZONE_H
#define TIMEZONE_H

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API


#if !UCONFIG_NO_FORMATTING

#include "unicode/uobject.h"
#include "unicode/unistr.h"
#include "unicode/ures.h"
#include "unicode/ucal.h"

U_NAMESPACE_BEGIN

class StringEnumeration;

class U_I18N_API TimeZone : public UObject {
public:
    virtual ~TimeZone();

    static const TimeZone& U_EXPORT2 getUnknown();

    static const TimeZone* U_EXPORT2 getGMT();

    static TimeZone* U_EXPORT2 createTimeZone(const UnicodeString& ID);

    static StringEnumeration* U_EXPORT2 createTimeZoneIDEnumeration(
        USystemTimeZoneType zoneType,
        const char* region,
        const int32_t* rawOffset,
        UErrorCode& ec);

#ifndef U_HIDE_DEPRECATED_API
    static StringEnumeration* U_EXPORT2 createEnumeration();
#endif  // U_HIDE_DEPRECATED_API

    static StringEnumeration* U_EXPORT2 createEnumeration(UErrorCode& status);

#ifndef U_HIDE_DEPRECATED_API
    static StringEnumeration* U_EXPORT2 createEnumeration(int32_t rawOffset);
#endif  // U_HIDE_DEPRECATED_API

    static StringEnumeration* U_EXPORT2 createEnumerationForRawOffset(int32_t rawOffset, UErrorCode& status);

#ifndef U_HIDE_DEPRECATED_API
    static StringEnumeration* U_EXPORT2 createEnumeration(const char* region);
#endif  // U_HIDE_DEPRECATED_API

    static StringEnumeration* U_EXPORT2 createEnumerationForRegion(const char* region, UErrorCode& status);

    static int32_t U_EXPORT2 countEquivalentIDs(const UnicodeString& id);

    static UnicodeString U_EXPORT2 getEquivalentID(const UnicodeString& id,
                                               int32_t index);

    static TimeZone* U_EXPORT2 detectHostTimeZone();

    static TimeZone* U_EXPORT2 createDefault();

#ifndef U_HIDE_INTERNAL_API
    static TimeZone* U_EXPORT2 forLocaleOrDefault(const Locale& locale);
#endif  /* U_HIDE_INTERNAL_API */

    static void U_EXPORT2 adoptDefault(TimeZone* zone);

#ifndef U_HIDE_SYSTEM_API
    static void U_EXPORT2 setDefault(const TimeZone& zone);
#endif  /* U_HIDE_SYSTEM_API */

    static const char* U_EXPORT2 getTZDataVersion(UErrorCode& status);

    static UnicodeString& U_EXPORT2 getCanonicalID(const UnicodeString& id,
        UnicodeString& canonicalID, UErrorCode& status);

    static UnicodeString& U_EXPORT2 getCanonicalID(const UnicodeString& id,
        UnicodeString& canonicalID, UBool& isSystemID, UErrorCode& status);


    static UnicodeString& U_EXPORT2 getIanaID(const UnicodeString&id, UnicodeString& ianaID,
        UErrorCode& status);

    static UnicodeString& U_EXPORT2 getWindowsID(const UnicodeString& id,
        UnicodeString& winid, UErrorCode& status);

    static UnicodeString& U_EXPORT2 getIDForWindowsID(const UnicodeString& winid, const char* region,
        UnicodeString& id, UErrorCode& status);

    virtual bool operator==(const TimeZone& that) const;

    bool operator!=(const TimeZone& that) const {return !operator==(that);}

    virtual int32_t getOffset(uint8_t era, int32_t year, int32_t month, int32_t day,
                              uint8_t dayOfWeek, int32_t millis, UErrorCode& status) const = 0;

    virtual int32_t getOffset(uint8_t era, int32_t year, int32_t month, int32_t day,
                           uint8_t dayOfWeek, int32_t milliseconds,
                           int32_t monthLength, UErrorCode& status) const = 0;

    virtual void getOffset(UDate date, UBool local, int32_t& rawOffset,
                           int32_t& dstOffset, UErrorCode& ec) const;

    virtual void setRawOffset(int32_t offsetMillis) = 0;

    virtual int32_t getRawOffset() const = 0;

    UnicodeString& getID(UnicodeString& ID) const;

    void setID(const UnicodeString& ID);

    enum EDisplayType {
        SHORT = 1,
        LONG,
        SHORT_GENERIC,
        LONG_GENERIC,
        SHORT_GMT,
        LONG_GMT,
        SHORT_COMMONLY_USED,
        GENERIC_LOCATION
    };

    UnicodeString& getDisplayName(UnicodeString& result) const;

    UnicodeString& getDisplayName(const Locale& locale, UnicodeString& result) const;

    UnicodeString& getDisplayName(UBool inDaylight, EDisplayType style, UnicodeString& result) const;

    UnicodeString& getDisplayName(UBool inDaylight, EDisplayType style, const Locale& locale, UnicodeString& result) const;
    
    virtual UBool useDaylightTime() const = 0;

#ifndef U_FORCE_HIDE_DEPRECATED_API
    virtual UBool inDaylightTime(UDate date, UErrorCode& status) const = 0;
#endif  // U_FORCE_HIDE_DEPRECATED_API

    virtual UBool hasSameRules(const TimeZone& other) const;

    virtual TimeZone* clone() const = 0;

    static UClassID U_EXPORT2 getStaticClassID();

    virtual UClassID getDynamicClassID() const override = 0;

    virtual int32_t getDSTSavings() const;

    static int32_t U_EXPORT2 getRegion(const UnicodeString& id, 
        char *region, int32_t capacity, UErrorCode& status); 

protected:

    TimeZone();

    TimeZone(const UnicodeString &id);

    TimeZone(const TimeZone& source);

    TimeZone& operator=(const TimeZone& right);

#ifndef U_HIDE_INTERNAL_API
    static UResourceBundle* loadRule(const UResourceBundle* top, const UnicodeString& ruleid, UResourceBundle* oldbundle, UErrorCode&status);
#endif  /* U_HIDE_INTERNAL_API */

private:
    friend class ZoneMeta;


    static TimeZone*        createCustomTimeZone(const UnicodeString&); 

    static const char16_t* findID(const UnicodeString& id);

    static const char16_t* dereferOlsonLink(const UnicodeString& id);

    static const char16_t* getRegion(const UnicodeString& id);

  public:
#ifndef U_HIDE_INTERNAL_API
    static const char16_t* getRegion(const UnicodeString& id, UErrorCode& status);
#endif  /* U_HIDE_INTERNAL_API */

  private:
    static UBool parseCustomID(const UnicodeString& id, int32_t& sign, int32_t& hour,
        int32_t& minute, int32_t& second);

    static UnicodeString& getCustomID(const UnicodeString& id, UnicodeString& normalized,
        UErrorCode& status);

    static UnicodeString& formatCustomID(int32_t hour, int32_t min, int32_t sec,
        UBool negative, UnicodeString& id);

    UnicodeString           fID;    

    friend class TZEnumeration;
};



inline UnicodeString&
TimeZone::getID(UnicodeString& ID) const
{
    ID = fID;
    return ID;
}


inline void
TimeZone::setID(const UnicodeString& ID)
{
    fID = ID;
}
U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */

#endif /* U_SHOW_CPLUSPLUS_API */

#endif //_TIMEZONE
