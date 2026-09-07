// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
* Copyright (C) 2003 - 2009, International Business Machines Corporation and  *
* others. All Rights Reserved.                                                *
*******************************************************************************
*/

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING

#include "cecal.h"
#include "gregoimp.h"   //Math
#include "cstring.h"

U_NAMESPACE_BEGIN

static const int32_t LIMITS[UCAL_FIELD_COUNT][4] = {
    {        0,        0,        1,        1}, 
    {        1,        1,  5000000,  5000000}, 
    {        0,        0,       12,       12}, 
    {        1,        1,       52,       53}, 
    {-1,-1,-1,-1}, 
    {        1,        1,        5,       30}, 
    {        1,        1,      365,      366}, 
    {-1,-1,-1,-1}, 
    {       -1,       -1,        1,        5}, 
    {-1,-1,-1,-1}, 
    {-1,-1,-1,-1}, 
    {-1,-1,-1,-1}, 
    {-1,-1,-1,-1}, 
    {-1,-1,-1,-1}, 
    {-1,-1,-1,-1}, 
    {-1,-1,-1,-1}, 
    {-1,-1,-1,-1}, 
    { -5000000, -5000000,  5000000,  5000000}, 
    {-1,-1,-1,-1}, 
    { -5000000, -5000000,  5000000,  5000000}, 
    {-1,-1,-1,-1}, 
    {-1,-1,-1,-1}, 
    {-1,-1,-1,-1}, 
    {        0,        0,       12,       12}, 
};


CECalendar::CECalendar(const Locale& aLocale, UErrorCode& success)
:   Calendar(TimeZone::forLocaleOrDefault(aLocale), aLocale, success)
{
}

CECalendar::CECalendar (const CECalendar& other) 
:   Calendar(other)
{
}

CECalendar::~CECalendar()
{
}


int64_t
CECalendar::handleComputeMonthStart(int32_t eyear,int32_t emonth, UBool , UErrorCode& ) const
{
    int64_t year64 = eyear;
    if ( emonth >= 0 ) {
        year64 += emonth/13;
        emonth %= 13;
    } else {
        ++emonth;
        year64 += emonth/13 - 1;
        emonth = emonth%13 + 12;
    }

    return (
        getJDEpochOffset()                    
        + 365LL * year64                      
        + ClockMath::floorDivideInt64(year64, 4LL) 
        + 30 * emonth                         
        - 1                                   
        );
}

int32_t
CECalendar::handleGetLimit(UCalendarDateFields field, ELimitType limitType) const
{
    return LIMITS[field][limitType];
}


namespace {
void jdToCE(int32_t julianDay, int32_t jdEpochOffset, int32_t& year, int32_t& month, int32_t& day, int32_t& doy, UErrorCode& status)
{
    int32_t c4; 
    int32_t r4; 

    if (uprv_add32_overflow(julianDay, -jdEpochOffset, &julianDay)) {
        status = U_ILLEGAL_ARGUMENT_ERROR;
        return;
    }
    c4 = ClockMath::floorDivide(julianDay, 1461, &r4);

    year = 4 * c4 + (r4/365 - r4/1460); 

    doy = (r4 == 1460) ? 365 : (r4 % 365); 

    month = doy / 30;       
    day = (doy % 30) + 1;   
    doy++;  
}
}  

void
CECalendar::handleComputeFields(int32_t julianDay, UErrorCode& status)
{
    int32_t eyear, month, day, doy;
    jdToCE(julianDay, getJDEpochOffset(), eyear, month, day, doy, status);
    if (U_FAILURE(status)) return;
    int32_t era = extendedYearToEra(eyear);
    int32_t year = extendedYearToYear(eyear);

    internalSet(UCAL_EXTENDED_YEAR, eyear);
    internalSet(UCAL_ERA, era);
    internalSet(UCAL_YEAR, year);
    internalSet(UCAL_MONTH, month);
    internalSet(UCAL_ORDINAL_MONTH, month);
    internalSet(UCAL_DATE, day);
    internalSet(UCAL_DAY_OF_YEAR, doy);
}

static const char* kMonthCode13 = "M13";

const char* CECalendar::getTemporalMonthCode(UErrorCode& status) const {
    if (get(UCAL_MONTH, status) == 12) {
        return kMonthCode13;
    }
    return Calendar::getTemporalMonthCode(status);
}

void
CECalendar::setTemporalMonthCode(const char* code, UErrorCode& status) {
    if (U_FAILURE(status)) {
        return;
    }
    if (uprv_strcmp(code, kMonthCode13) == 0) {
        set(UCAL_MONTH, 12);
        set(UCAL_IS_LEAP_MONTH, 0);
        return;
    }
    Calendar::setTemporalMonthCode(code, status);
}

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */
