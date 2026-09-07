// License & terms of use: http://www.unicode.org/copyright.html
/*
* Copyright (C) 1997-2013, International Business Machines Corporation and others.
* All Rights Reserved.
********************************************************************************
*
* File GREGOCAL.H
*
* Modification History:
*
*   Date        Name        Description
*   04/22/97    aliu        Overhauled header.
*    07/28/98    stephen        Sync with JDK 1.2
*    09/04/98    stephen        Re-sync with JDK 8/31 putback
*    09/14/98    stephen        Changed type of kOneDay, kOneWeek to double.
*                            Fixed bug in roll()
*   10/15/99    aliu        Fixed j31, incorrect WEEK_OF_YEAR computation.
*                           Added documentation of WEEK_OF_YEAR computation.
*   10/15/99    aliu        Fixed j32, cannot set date to Feb 29 2000 AD.
*                           {JDK bug 4210209 4209272}
*   11/07/2003  srl         Update, clean up documentation.
********************************************************************************
*/

#ifndef GREGOCAL_H
#define GREGOCAL_H

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API

#if !UCONFIG_NO_FORMATTING

#include "unicode/calendar.h"


U_NAMESPACE_BEGIN

class U_I18N_API GregorianCalendar: public Calendar {
public:

    enum EEras {
        BC,
        AD
    };

    GregorianCalendar(UErrorCode& success);

    GregorianCalendar(TimeZone* zoneToAdopt, UErrorCode& success);

    GregorianCalendar(const TimeZone& zone, UErrorCode& success);

    GregorianCalendar(const Locale& aLocale, UErrorCode& success);

    GregorianCalendar(TimeZone* zoneToAdopt, const Locale& aLocale, UErrorCode& success);

    GregorianCalendar(const TimeZone& zone, const Locale& aLocale, UErrorCode& success);

    GregorianCalendar(int32_t year, int32_t month, int32_t date, UErrorCode& success);

    GregorianCalendar(int32_t year, int32_t month, int32_t date, int32_t hour, int32_t minute, UErrorCode& success);

    GregorianCalendar(int32_t year, int32_t month, int32_t date, int32_t hour, int32_t minute, int32_t second, UErrorCode& success);

    virtual ~GregorianCalendar();

    GregorianCalendar(const GregorianCalendar& source);

    GregorianCalendar& operator=(const GregorianCalendar& right);

    virtual GregorianCalendar* clone() const override;

    void setGregorianChange(UDate date, UErrorCode& success);

    UDate getGregorianChange() const;

    UBool isLeapYear(int32_t year) const;

    virtual UBool isEquivalentTo(const Calendar& other) const override;

#ifndef U_FORCE_HIDE_DEPRECATED_API
    virtual void roll(EDateFields field, int32_t amount, UErrorCode& status) override;
#endif  // U_FORCE_HIDE_DEPRECATED_API

    virtual void roll(UCalendarDateFields field, int32_t amount, UErrorCode& status) override;

#ifndef U_HIDE_DEPRECATED_API
    int32_t getActualMinimum(EDateFields field) const;

    int32_t getActualMinimum(EDateFields field, UErrorCode& status) const;
#endif  /* U_HIDE_DEPRECATED_API */

    int32_t getActualMinimum(UCalendarDateFields field, UErrorCode &status) const override;

    virtual int32_t getActualMaximum(UCalendarDateFields field, UErrorCode& status) const override;

public:

    virtual UClassID getDynamicClassID() const override;

    static UClassID U_EXPORT2 getStaticClassID();

    virtual const char * getType() const override;

 private:
    GregorianCalendar() = delete; 

 protected:
    virtual int32_t internalGetEra() const;

    virtual int64_t handleComputeMonthStart(int32_t eyear, int32_t month,
                                            UBool useMonth, UErrorCode& status) const override;

    virtual int32_t handleComputeJulianDay(UCalendarDateFields bestField, UErrorCode& status) override;

    virtual int32_t handleGetMonthLength(int32_t extendedYear, int32_t month, UErrorCode& status) const override;

    virtual int32_t handleGetYearLength(int32_t eyear, UErrorCode& status) const override;

    virtual int32_t monthLength(int32_t month, UErrorCode& status) const;

    virtual int32_t monthLength(int32_t month, int32_t year) const;

#ifndef U_HIDE_INTERNAL_API
  int32_t yearLength() const;

#endif  /* U_HIDE_INTERNAL_API */

    virtual UDate getEpochDay(UErrorCode& status);

    virtual int32_t handleGetLimit(UCalendarDateFields field, ELimitType limitType) const override;

    virtual int32_t handleGetExtendedYear(UErrorCode& status) override;

    virtual int32_t handleGetExtendedYearFromWeekFields(int32_t yearWoy, int32_t woy, UErrorCode& status) override;


    virtual void handleComputeFields(int32_t julianDay, UErrorCode &status) override;

#ifndef U_HIDE_INTERNAL_API
    virtual bool isEra0CountingBackward() const override { return true; }
#endif  // U_HIDE_INTERNAL_API

 private:
    static double computeJulianDayOfYear(UBool isGregorian, int32_t year,
                                         UBool& isLeap);
    
    UBool validateFields() const;

    UBool boundsCheck(int32_t value, UCalendarDateFields field) const;

    int32_t aggregateStamp(int32_t stamp_a, int32_t stamp_b);

    UDate                fGregorianCutover;

    int32_t             fCutoverJulianDay;

    int32_t fGregorianCutoverYear;

    static double millisToJulianDay(UDate millis);

    static UDate julianDayToMillis(double julian);

    UBool fIsGregorian;

    UBool fInvertGregorian;


 public: 

    DECLARE_OVERRIDE_SYSTEM_DEFAULT_CENTURY

};

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */

#endif /* U_SHOW_CPLUSPLUS_API */

#endif // _GREGOCAL

