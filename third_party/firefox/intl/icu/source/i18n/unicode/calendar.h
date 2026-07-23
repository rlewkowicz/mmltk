// License & terms of use: http://www.unicode.org/copyright.html
/*
********************************************************************************
*   Copyright (C) 1997-2014, International Business Machines
*   Corporation and others.  All Rights Reserved.
********************************************************************************
*
* File CALENDAR.H
*
* Modification History:
*
*   Date        Name        Description
*   04/22/97    aliu        Expanded and corrected comments and other header
*                           contents.
*   05/01/97    aliu        Made equals(), before(), after() arguments const.
*   05/20/97    aliu        Replaced fAreFieldsSet with fAreFieldsInSync and
*                           fAreAllFieldsSet.
*   07/27/98    stephen     Sync up with JDK 1.2
*   11/15/99    weiv        added YEAR_WOY and DOW_LOCAL
*                           to EDateFields
*    8/19/2002  srl         Removed Javaisms
*   11/07/2003  srl         Update, clean up documentation.
********************************************************************************
*/

#ifndef CALENDAR_H
#define CALENDAR_H

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API

#if !UCONFIG_NO_FORMATTING

#include "unicode/uobject.h"
#include "unicode/locid.h"
#include "unicode/timezone.h"
#include "unicode/ucal.h"
#include "unicode/umisc.h"

U_NAMESPACE_BEGIN

class ICUServiceFactory;

typedef int32_t UFieldResolutionTable[12][8];

class BasicTimeZone;
class U_I18N_API_CLASS Calendar : public UObject {
public:
#ifndef U_FORCE_HIDE_DEPRECATED_API
    enum EDateFields {
#ifndef U_HIDE_DEPRECATED_API
#ifdef ERA
#undef ERA
#endif
        ERA,                  
        YEAR,                 
        MONTH,                
        WEEK_OF_YEAR,         
        WEEK_OF_MONTH,        
        DATE,                 
        DAY_OF_YEAR,          
        DAY_OF_WEEK,          
        DAY_OF_WEEK_IN_MONTH, 
        AM_PM,                
        HOUR,                 
        HOUR_OF_DAY,          
        MINUTE,               
        SECOND,               
        MILLISECOND,          
        ZONE_OFFSET,          
        DST_OFFSET,           
        YEAR_WOY,             
        DOW_LOCAL,            

        EXTENDED_YEAR,
        JULIAN_DAY,
        MILLISECONDS_IN_DAY,
        IS_LEAP_MONTH,

        FIELD_COUNT = UCAL_FIELD_COUNT 
#endif /* U_HIDE_DEPRECATED_API */
    };
#endif  // U_FORCE_HIDE_DEPRECATED_API

#ifndef U_HIDE_DEPRECATED_API
    enum EDaysOfWeek {
        SUNDAY = 1,
        MONDAY,
        TUESDAY,
        WEDNESDAY,
        THURSDAY,
        FRIDAY,
        SATURDAY
    };

    enum EMonths {
        JANUARY,
        FEBRUARY,
        MARCH,
        APRIL,
        MAY,
        JUNE,
        JULY,
        AUGUST,
        SEPTEMBER,
        OCTOBER,
        NOVEMBER,
        DECEMBER,
        UNDECIMBER
    };

    enum EAmpm {
        AM,
        PM
    };
#endif  /* U_HIDE_DEPRECATED_API */

    U_I18N_API virtual ~Calendar();

    U_I18N_API virtual Calendar* clone() const = 0;

    U_I18N_API static Calendar* U_EXPORT2 createInstance(UErrorCode& success);

    U_I18N_API static Calendar* U_EXPORT2 createInstance(TimeZone* zoneToAdopt, UErrorCode& success);

    U_I18N_API static Calendar* U_EXPORT2 createInstance(const TimeZone& zone, UErrorCode& success);

    U_I18N_API static Calendar* U_EXPORT2 createInstance(const Locale& aLocale, UErrorCode& success);

    U_I18N_API static Calendar* U_EXPORT2 createInstance(TimeZone* zoneToAdopt,
                                                         const Locale& aLocale,
                                                         UErrorCode& success);

    U_I18N_API static Calendar* U_EXPORT2 createInstance(const TimeZone& zone,
                                                         const Locale& aLocale,
                                                         UErrorCode& success);

    U_I18N_API static const Locale* U_EXPORT2 getAvailableLocales(int32_t& count);

    U_I18N_API static StringEnumeration* U_EXPORT2 getKeywordValuesForLocale(const char* key,
                                                                             const Locale& locale,
                                                                             UBool commonlyUsed,
                                                                             UErrorCode& status);

    U_I18N_API static UDate U_EXPORT2 getNow();

    U_I18N_API inline UDate getTime(UErrorCode& status) const { return getTimeInMillis(status); }

    U_I18N_API inline void setTime(UDate date, UErrorCode& status) { setTimeInMillis(date, status); }

    U_I18N_API virtual bool operator==(const Calendar& that) const;

    U_I18N_API bool operator!=(const Calendar& that) const { return !operator==(that); }

    U_I18N_API virtual UBool isEquivalentTo(const Calendar& other) const;

    U_I18N_API UBool equals(const Calendar& when, UErrorCode& status) const;

    U_I18N_API UBool before(const Calendar& when, UErrorCode& status) const;

    U_I18N_API UBool after(const Calendar& when, UErrorCode& status) const;

#ifndef U_FORCE_HIDE_DEPRECATED_API
    U_I18N_API virtual void add(EDateFields field, int32_t amount, UErrorCode& status);
#endif  // U_FORCE_HIDE_DEPRECATED_API

    U_I18N_API virtual void add(UCalendarDateFields field, int32_t amount, UErrorCode& status);

#ifndef U_HIDE_DEPRECATED_API
    U_I18N_API inline void roll(EDateFields field, UBool up, UErrorCode& status);
#endif  /* U_HIDE_DEPRECATED_API */

    U_I18N_API inline void roll(UCalendarDateFields field, UBool up, UErrorCode& status);

#ifndef U_FORCE_HIDE_DEPRECATED_API
    U_I18N_API virtual void roll(EDateFields field, int32_t amount, UErrorCode& status);
#endif  // U_FORCE_HIDE_DEPRECATED_API

    U_I18N_API virtual void roll(UCalendarDateFields field, int32_t amount, UErrorCode& status);

#ifndef U_FORCE_HIDE_DEPRECATED_API
    U_I18N_API virtual int32_t fieldDifference(UDate when, EDateFields field, UErrorCode& status);
#endif  // U_FORCE_HIDE_DEPRECATED_API

    U_I18N_API virtual int32_t fieldDifference(UDate when,
                                               UCalendarDateFields field,
                                               UErrorCode& status);

    U_I18N_API void adoptTimeZone(TimeZone* value);

    U_I18N_API void setTimeZone(const TimeZone& zone);

    U_I18N_API const TimeZone& getTimeZone() const;

    U_I18N_API TimeZone* orphanTimeZone();

    U_I18N_API virtual UBool inDaylightTime(UErrorCode& status) const;

    U_I18N_API void setLenient(UBool lenient);

    U_I18N_API UBool isLenient() const;

    U_I18N_API void setRepeatedWallTimeOption(UCalendarWallTimeOption option);

    U_I18N_API UCalendarWallTimeOption getRepeatedWallTimeOption() const;

    U_I18N_API void setSkippedWallTimeOption(UCalendarWallTimeOption option);

    U_I18N_API UCalendarWallTimeOption getSkippedWallTimeOption() const;

    U_I18N_API void setFirstDayOfWeek(UCalendarDaysOfWeek value);

#ifndef U_HIDE_DEPRECATED_API
    U_I18N_API EDaysOfWeek getFirstDayOfWeek() const;
#endif  /* U_HIDE_DEPRECATED_API */

    U_I18N_API UCalendarDaysOfWeek getFirstDayOfWeek(UErrorCode& status) const;

    U_I18N_API void setMinimalDaysInFirstWeek(uint8_t value);

    U_I18N_API uint8_t getMinimalDaysInFirstWeek() const;

#ifndef U_FORCE_HIDE_DEPRECATED_API
    U_I18N_API virtual int32_t getMinimum(EDateFields field) const;
#endif  // U_FORCE_HIDE_DEPRECATED_API

    U_I18N_API virtual int32_t getMinimum(UCalendarDateFields field) const;

#ifndef U_FORCE_HIDE_DEPRECATED_API
    U_I18N_API virtual int32_t getMaximum(EDateFields field) const;
#endif  // U_FORCE_HIDE_DEPRECATED_API

    U_I18N_API virtual int32_t getMaximum(UCalendarDateFields field) const;

#ifndef U_FORCE_HIDE_DEPRECATED_API
    U_I18N_API virtual int32_t getGreatestMinimum(EDateFields field) const;
#endif  // U_FORCE_HIDE_DEPRECATED_API

    U_I18N_API virtual int32_t getGreatestMinimum(UCalendarDateFields field) const;

#ifndef U_FORCE_HIDE_DEPRECATED_API
    U_I18N_API virtual int32_t getLeastMaximum(EDateFields field) const;
#endif  // U_FORCE_HIDE_DEPRECATED_API

    U_I18N_API virtual int32_t getLeastMaximum(UCalendarDateFields field) const;

#ifndef U_HIDE_DEPRECATED_API
    U_I18N_API int32_t getActualMinimum(EDateFields field, UErrorCode& status) const;
#endif  /* U_HIDE_DEPRECATED_API */

    U_I18N_API virtual int32_t getActualMinimum(UCalendarDateFields field, UErrorCode& status) const;

    U_I18N_API virtual int32_t getActualMaximum(UCalendarDateFields field, UErrorCode& status) const;

    U_I18N_API int32_t get(UCalendarDateFields field, UErrorCode& status) const;

    U_I18N_API UBool isSet(UCalendarDateFields field) const;

    U_I18N_API void set(UCalendarDateFields field, int32_t value);

    U_I18N_API void set(int32_t year, int32_t month, int32_t date);

    U_I18N_API void set(int32_t year, int32_t month, int32_t date, int32_t hour, int32_t minute);

    U_I18N_API void set(int32_t year, int32_t month, int32_t date,
                        int32_t hour, int32_t minute, int32_t second);

    U_I18N_API void clear();

    U_I18N_API void clear(UCalendarDateFields field);

    U_I18N_API virtual UClassID getDynamicClassID() const override = 0;

    U_I18N_API virtual const char* getType() const = 0;

    U_I18N_API virtual UCalendarWeekdayType getDayOfWeekType(UCalendarDaysOfWeek dayOfWeek,
                                                             UErrorCode& status) const;

    U_I18N_API virtual int32_t getWeekendTransition(UCalendarDaysOfWeek dayOfWeek,
                                                    UErrorCode& status) const;

    U_I18N_API virtual UBool isWeekend(UDate date, UErrorCode& status) const;

    U_I18N_API virtual UBool isWeekend() const;

    U_I18N_API virtual bool inTemporalLeapYear(UErrorCode& status) const;

    U_I18N_API virtual const char* getTemporalMonthCode(UErrorCode& status) const;

    U_I18N_API virtual void setTemporalMonthCode(const char* temporalMonth, UErrorCode& status);

protected:

    U_I18N_API Calendar(UErrorCode& success);

    U_I18N_API Calendar(const Calendar& source);

    U_I18N_API Calendar& operator=(const Calendar& right);

    U_I18N_API Calendar(TimeZone* zone, const Locale& aLocale, UErrorCode& success);

    U_I18N_API Calendar(const TimeZone& zone, const Locale& aLocale, UErrorCode& success);

    U_I18N_API virtual void computeTime(UErrorCode& status);

    U_I18N_API virtual void computeFields(UErrorCode& status);

    U_I18N_API double getTimeInMillis(UErrorCode& status) const;

    U_I18N_API void setTimeInMillis(double millis, UErrorCode& status);

    U_I18N_API void complete(UErrorCode& status);

#ifndef U_HIDE_DEPRECATED_API
    U_I18N_API inline int32_t internalGet(EDateFields field) const { return fFields[field]; }
#endif  /* U_HIDE_DEPRECATED_API */

#ifndef U_HIDE_INTERNAL_API
    U_I18N_API inline int32_t internalGet(UCalendarDateFields field, int32_t defaultValue) const {
        return fStamp[field] > kUnset ? fFields[field] : defaultValue;
    }

    U_I18N_API inline int32_t internalGet(UCalendarDateFields field) const { return fFields[field]; }

    U_I18N_API virtual bool isEra0CountingBackward() const { return false; }

    U_I18N_API virtual int32_t getRelatedYearDifference() const;

#endif  /* U_HIDE_INTERNAL_API */

    U_I18N_API virtual int32_t internalGetMonth(UErrorCode& status) const;

    U_I18N_API virtual int32_t internalGetMonth(int32_t defaultValue, UErrorCode& status) const;

#ifndef U_HIDE_DEPRECATED_API
    U_I18N_API void internalSet(EDateFields field, int32_t value);
#endif  /* U_HIDE_DEPRECATED_API */

    U_I18N_API inline void internalSet(UCalendarDateFields field, int32_t value);

    U_I18N_API virtual void prepareGetActual(UCalendarDateFields field,
                                             UBool isMinimum,
                                             UErrorCode& status);

    enum ELimitType {
#ifndef U_HIDE_INTERNAL_API
      UCAL_LIMIT_MINIMUM = 0,
      UCAL_LIMIT_GREATEST_MINIMUM,
      UCAL_LIMIT_LEAST_MAXIMUM,
      UCAL_LIMIT_MAXIMUM,
      UCAL_LIMIT_COUNT
#endif  /* U_HIDE_INTERNAL_API */
    };

    U_I18N_API virtual int32_t handleGetLimit(UCalendarDateFields field, ELimitType limitType) const = 0;

    U_I18N_API virtual int32_t getLimit(UCalendarDateFields field, ELimitType limitType) const;

    U_I18N_API virtual int64_t handleComputeMonthStart(int32_t eyear,
                                                       int32_t month,
                                                       UBool useMonth,
                                                       UErrorCode& status) const = 0;

    U_I18N_API virtual int32_t handleGetMonthLength(int32_t extendedYear,
                                                    int32_t month,
                                                    UErrorCode& status) const;

    U_I18N_API virtual int32_t handleGetYearLength(int32_t eyear, UErrorCode& status) const;

    U_I18N_API virtual int32_t handleGetExtendedYear(UErrorCode& status) = 0;

    U_I18N_API virtual int32_t handleComputeJulianDay(UCalendarDateFields bestField, UErrorCode& status);

    U_I18N_API virtual int32_t handleGetExtendedYearFromWeekFields(int32_t yearWoy,
                                                                   int32_t woy,
                                                                   UErrorCode& status);

    U_I18N_API virtual void validateField(UCalendarDateFields field, UErrorCode& status);

#ifndef U_HIDE_INTERNAL_API
    U_I18N_API int32_t computeJulianDay(UErrorCode& status);

    U_I18N_API double computeMillisInDay();

    U_I18N_API int32_t computeZoneOffset(double millis, double millisInDay, UErrorCode& ec);

    U_I18N_API int32_t newestStamp(UCalendarDateFields start,
                                   UCalendarDateFields end,
                                   int32_t bestSoFar) const;

    U_I18N_API static constexpr int32_t kResolveSTOP = -1;

    U_I18N_API static constexpr int32_t kResolveRemap = 32;

    U_I18N_API static const UFieldResolutionTable kDatePrecedence[];

    U_I18N_API static const UFieldResolutionTable kYearPrecedence[];

    U_I18N_API static const UFieldResolutionTable kDOWPrecedence[];

    U_I18N_API static const UFieldResolutionTable kMonthPrecedence[];

    U_I18N_API UCalendarDateFields resolveFields(const UFieldResolutionTable* precedenceTable) const;
#endif  /* U_HIDE_INTERNAL_API */

    U_I18N_API virtual const UFieldResolutionTable* getFieldResolutionTable() const;

#ifndef U_HIDE_INTERNAL_API
    U_I18N_API UCalendarDateFields newerField(UCalendarDateFields defaultField,
                                              UCalendarDateFields alternateField) const;
#endif  /* U_HIDE_INTERNAL_API */


private:
    int32_t getActualHelper(UCalendarDateFields field, int32_t startValue, int32_t endValue, UErrorCode &status) const;

protected:
    U_I18N_API UDate internalGetTime() const { return fTime; }

    U_I18N_API void internalSetTime(UDate time) { fTime = time; }

    int32_t     fFields[UCAL_FIELD_COUNT];

protected:
    enum {
        kUnset                 = 0,
        kInternallySet,
        kMinimumUserStamp
    };

private:
    int8_t        fStamp[UCAL_FIELD_COUNT];

protected:
  U_I18N_API virtual void handleComputeFields(int32_t julianDay, UErrorCode& status);

#ifndef U_HIDE_INTERNAL_API
    U_I18N_API int32_t getGregorianYear() const {
        return fGregorianYear;
    }

    U_I18N_API int32_t getGregorianMonth() const {
        return fGregorianMonth;
    }

    U_I18N_API int32_t getGregorianDayOfYear() const {
        return fGregorianDayOfYear;
    }

    U_I18N_API int32_t getGregorianDayOfMonth() const {
      return fGregorianDayOfMonth;
    }
#endif  /* U_HIDE_INTERNAL_API */

    U_I18N_API virtual int32_t getDefaultMonthInYear(int32_t eyear, UErrorCode& status);

    U_I18N_API virtual int32_t getDefaultDayInMonth(int32_t eyear, int32_t month, UErrorCode& status);


    U_I18N_API virtual void pinField(UCalendarDateFields field, UErrorCode& status);

    U_I18N_API int32_t weekNumber(int32_t desiredDay, int32_t dayOfPeriod, int32_t dayOfWeek);

#ifndef U_HIDE_INTERNAL_API
    U_I18N_API inline int32_t weekNumber(int32_t dayOfPeriod, int32_t dayOfWeek);

    U_I18N_API int32_t getLocalDOW(UErrorCode& status);
#endif  /* U_HIDE_INTERNAL_API */

private:

    int8_t fNextStamp = kMinimumUserStamp;

    void recalculateStamp();

    UDate        fTime = 0;

    TimeZone*   fZone = nullptr;

    bool      fIsTimeSet:1;

    bool      fAreFieldsSet:1;

    bool      fAreAllFieldsSet:1;

    bool      fAreFieldsVirtuallySet:1;

    bool      fLenient:1;

    UCalendarWallTimeOption fRepeatedWallTime:3; 

    UCalendarWallTimeOption fSkippedWallTime:3; 

    UCalendarDaysOfWeek fFirstDayOfWeek:4; 
    UCalendarDaysOfWeek fWeekendOnset:4; 
    UCalendarDaysOfWeek fWeekendCease:4; 
    uint8_t fMinimalDaysInFirstWeek;
    int32_t fWeekendOnsetMillis;
    int32_t fWeekendCeaseMillis;

    void        setWeekData(const Locale& desiredLocale, const char *type, UErrorCode& success);

    void updateTime(UErrorCode& status);

    int32_t fGregorianYear;

    int8_t fGregorianMonth;

    int8_t fGregorianDayOfMonth;

    int16_t fGregorianDayOfYear;


protected:

    U_I18N_API void computeGregorianFields(int32_t julianDay, UErrorCode& ec);

private:

    void computeWeekFields(UErrorCode &ec);


    void validateFields(UErrorCode &status);

    void validateField(UCalendarDateFields field, int32_t min, int32_t max, UErrorCode& status);

 protected:
#ifndef U_HIDE_INTERNAL_API
    U_I18N_API static uint8_t julianDayToDayOfWeek(int32_t julian);
#endif  /* U_HIDE_INTERNAL_API */

 private:
    Locale validLocale;
    Locale actualLocale;

 public:
#if !UCONFIG_NO_SERVICE

#ifndef U_HIDE_INTERNAL_API
    U_I18N_API static StringEnumeration* getAvailableLocales();

    U_I18N_API static URegistryKey registerFactory(ICUServiceFactory* toAdopt, UErrorCode& status);

    U_I18N_API static UBool unregister(URegistryKey key, UErrorCode& status);
#endif  /* U_HIDE_INTERNAL_API */

    friend class CalendarFactory;

    friend class CalendarService;

    friend class DefaultCalendarFactory;
#endif /* !UCONFIG_NO_SERVICE */

    U_I18N_API virtual UBool haveDefaultCentury() const = 0;

    U_I18N_API virtual UDate defaultCenturyStart() const = 0;

    U_I18N_API virtual int32_t defaultCenturyStartYear() const = 0;

    U_I18N_API Locale getLocale(ULocDataLocaleType type, UErrorCode& status) const;

    U_I18N_API virtual int32_t getRelatedYear(UErrorCode& status) const;

    U_I18N_API virtual void setRelatedYear(int32_t year);

#ifndef U_HIDE_INTERNAL_API
    U_I18N_API const char* getLocaleID(ULocDataLocaleType type, UErrorCode& status) const;
#endif  /* U_HIDE_INTERNAL_API */

private:
    BasicTimeZone* getBasicTimeZone() const;

    UBool getImmediatePreviousZoneTransition(UDate base, UDate *transitionTime, UErrorCode& status) const;

public:
#ifndef U_HIDE_INTERNAL_API
    U_I18N_API static Calendar* U_EXPORT2 makeInstance(const Locale& locale, UErrorCode& status);

    U_I18N_API static void U_EXPORT2 getCalendarTypeFromLocale(const Locale& locale,
                                                               char* typeBuffer,
                                                               int32_t typeBufferSize,
                                                               UErrorCode& status);
#endif  /* U_HIDE_INTERNAL_API */
};


inline Calendar*
Calendar::createInstance(TimeZone* zone, UErrorCode& errorCode)
{
    return createInstance(zone, Locale::getDefault(), errorCode);
}


inline void
Calendar::roll(UCalendarDateFields field, UBool up, UErrorCode& status)
{
    roll(field, static_cast<int32_t>(up ? +1 : -1), status);
}

#ifndef U_HIDE_DEPRECATED_API
inline void
Calendar::roll(EDateFields field, UBool up, UErrorCode& status)
{
    roll(static_cast<UCalendarDateFields>(field), up, status);
}
#endif  /* U_HIDE_DEPRECATED_API */




inline void
Calendar::internalSet(UCalendarDateFields field, int32_t value)
{
    fFields[field] = value;
    fStamp[field] = kInternallySet;
}

#define DECLARE_OVERRIDE_SYSTEM_DEFAULT_CENTURY \
    virtual UBool haveDefaultCentury() const override; \
    virtual UDate defaultCenturyStart() const override; \
    virtual int32_t defaultCenturyStartYear() const override;

#ifndef U_HIDE_INTERNAL_API
inline int32_t  Calendar::weekNumber(int32_t dayOfPeriod, int32_t dayOfWeek)
{
  return weekNumber(dayOfPeriod, dayOfPeriod, dayOfWeek);
}
#endif  /* U_HIDE_INTERNAL_API */

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */

#endif /* U_SHOW_CPLUSPLUS_API */

#endif // _CALENDAR
