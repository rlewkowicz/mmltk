// License & terms of use: http://www.unicode.org/copyright.html
/*
********************************************************************************
*   Copyright (C) 1997-2016, International Business Machines
*   Corporation and others.  All Rights Reserved.
********************************************************************************
*
* File DTFMTSYM.H
*
* Modification History:
*
*   Date        Name        Description
*   02/19/97    aliu        Converted from java.
*    07/21/98    stephen        Added getZoneIndex()
*                            Changed to match C++ conventions
********************************************************************************
*/

#ifndef DTFMTSYM_H
#define DTFMTSYM_H

#include "unicode/utypes.h"

#if U_SHOW_CPLUSPLUS_API

#if !UCONFIG_NO_FORMATTING

#include "unicode/calendar.h"
#include "unicode/strenum.h"
#include "unicode/uobject.h"
#include "unicode/locid.h"
#include "unicode/udat.h"
#include "unicode/ures.h"


U_NAMESPACE_BEGIN

class SimpleDateFormat;
class Hashtable;

class U_I18N_API_CLASS DateFormatSymbols final : public UObject  {
public:
    U_I18N_API DateFormatSymbols(UErrorCode& status);

    U_I18N_API DateFormatSymbols(const Locale& locale, UErrorCode& status);

#ifndef U_HIDE_INTERNAL_API
    U_I18N_API DateFormatSymbols(const char* type, UErrorCode& status);

    U_I18N_API DateFormatSymbols(const Locale& locale, const char* type, UErrorCode& status);
#endif  /* U_HIDE_INTERNAL_API */

    U_I18N_API DateFormatSymbols(const DateFormatSymbols&);

    U_I18N_API DateFormatSymbols& operator=(const DateFormatSymbols&);

    U_I18N_API virtual ~DateFormatSymbols();

    U_I18N_API bool operator==(const DateFormatSymbols& other) const;

    U_I18N_API bool operator!=(const DateFormatSymbols& other) const { return !operator==(other); }

    U_I18N_API const UnicodeString* getEras(int32_t& count) const;

    U_I18N_API void setEras(const UnicodeString* eras, int32_t count);

    U_I18N_API const UnicodeString* getEraNames(int32_t& count) const;

    U_I18N_API void setEraNames(const UnicodeString* eraNames, int32_t count);

    U_I18N_API const UnicodeString* getNarrowEras(int32_t& count) const;

    U_I18N_API void setNarrowEras(const UnicodeString* narrowEras, int32_t count);

    U_I18N_API const UnicodeString* getMonths(int32_t& count) const;

    U_I18N_API void setMonths(const UnicodeString* months, int32_t count);

    U_I18N_API const UnicodeString* getShortMonths(int32_t& count) const;

    U_I18N_API void setShortMonths(const UnicodeString* shortMonths, int32_t count);

    enum DtContextType {
        FORMAT,
        STANDALONE,
#ifndef U_HIDE_DEPRECATED_API
        DT_CONTEXT_COUNT
#endif  // U_HIDE_DEPRECATED_API
    };

    enum DtWidthType {
        ABBREVIATED,
        WIDE,
        NARROW,
        SHORT,
#ifndef U_HIDE_DEPRECATED_API
        DT_WIDTH_COUNT = 4
#endif  // U_HIDE_DEPRECATED_API
    };

    U_I18N_API const UnicodeString* getMonths(int32_t& count,
                                              DtContextType context,
                                              DtWidthType width) const;

    U_I18N_API void setMonths(const UnicodeString* months,
                              int32_t count,
                              DtContextType context,
                              DtWidthType width);

    U_I18N_API const UnicodeString* getWeekdays(int32_t& count) const;

    U_I18N_API void setWeekdays(const UnicodeString* weekdays, int32_t count);

    U_I18N_API const UnicodeString* getShortWeekdays(int32_t& count) const;

    U_I18N_API void setShortWeekdays(const UnicodeString* abbrevWeekdays, int32_t count);

    U_I18N_API const UnicodeString* getWeekdays(int32_t& count,
                                                DtContextType context,
                                                DtWidthType width) const;

    U_I18N_API void setWeekdays(const UnicodeString* weekdays,
                                int32_t count,
                                DtContextType context,
                                DtWidthType width);

    U_I18N_API const UnicodeString* getQuarters(int32_t& count,
                                                DtContextType context,
                                                DtWidthType width) const;

    U_I18N_API void setQuarters(const UnicodeString* quarters,
                                int32_t count,
                                DtContextType context,
                                DtWidthType width);

    U_I18N_API const UnicodeString* getAmPmStrings(int32_t& count) const;

    U_I18N_API void setAmPmStrings(const UnicodeString* ampms, int32_t count);

#ifndef U_HIDE_DRAFT_API
    U_I18N_API const UnicodeString* getAmPmStrings(int32_t& count,
                                                   DtContextType context,
                                                   DtWidthType width) const;

    U_I18N_API void setAmPmStrings(const UnicodeString* ampms,
                                   int32_t count,
                                   DtContextType context,
                                   DtWidthType width);
#endif  /* U_HIDE_DRAFT_API */

#ifndef U_HIDE_INTERNAL_API
    U_I18N_API static const char16_t DEFAULT_TIME_SEPARATOR = 0x003a;  

    U_I18N_API static const char16_t ALTERNATE_TIME_SEPARATOR = 0x002e;  

    U_I18N_API UnicodeString& getTimeSeparatorString(UnicodeString& result) const;

    U_I18N_API void setTimeSeparatorString(const UnicodeString& newTimeSeparator);
#endif  /* U_HIDE_INTERNAL_API */

    U_I18N_API const UnicodeString* getYearNames(int32_t& count,
                                                 DtContextType context,
                                                 DtWidthType width) const;

    U_I18N_API void setYearNames(const UnicodeString* yearNames,
                                 int32_t count,
                                 DtContextType context,
                                 DtWidthType width);

    U_I18N_API const UnicodeString* getZodiacNames(int32_t& count,
                                                   DtContextType context,
                                                   DtWidthType width) const;

    U_I18N_API void setZodiacNames(const UnicodeString* zodiacNames,
                                   int32_t count,
                                   DtContextType context,
                                   DtWidthType width);

#ifndef U_HIDE_INTERNAL_API
    enum EMonthPatternType
    {
        kLeapMonthPatternFormatWide,
        kLeapMonthPatternFormatAbbrev,
        kLeapMonthPatternFormatNarrow,
        kLeapMonthPatternStandaloneWide,
        kLeapMonthPatternStandaloneAbbrev,
        kLeapMonthPatternStandaloneNarrow,
        kLeapMonthPatternNumeric,
        kMonthPatternsCount
    };

    U_I18N_API const UnicodeString* getLeapMonthPatterns(int32_t& count) const;

#endif  /* U_HIDE_INTERNAL_API */

#ifndef U_HIDE_DEPRECATED_API
    U_I18N_API const UnicodeString** getZoneStrings(int32_t& rowCount, int32_t& columnCount) const;
#endif  /* U_HIDE_DEPRECATED_API */

    U_I18N_API void setZoneStrings(const UnicodeString* const* strings,
                                   int32_t rowCount,
                                   int32_t columnCount);

    U_I18N_API static const char16_t* getPatternUChars();

    U_I18N_API UnicodeString& getLocalPatternChars(UnicodeString& result) const;

    U_I18N_API void setLocalPatternChars(const UnicodeString& newLocalPatternChars);

    U_I18N_API Locale getLocale(ULocDataLocaleType type, UErrorCode& status) const;

    enum ECapitalizationContextUsageType
    {
#ifndef U_HIDE_INTERNAL_API
        kCapContextUsageOther = 0,
        kCapContextUsageMonthFormat,     
        kCapContextUsageMonthStandalone, 
        kCapContextUsageMonthNarrow,
        kCapContextUsageDayFormat,     
        kCapContextUsageDayStandalone, 
        kCapContextUsageDayNarrow,
        kCapContextUsageEraWide,
        kCapContextUsageEraAbbrev,
        kCapContextUsageEraNarrow,
        kCapContextUsageZoneLong,
        kCapContextUsageZoneShort,
        kCapContextUsageMetazoneLong,
        kCapContextUsageMetazoneShort,
#endif /* U_HIDE_INTERNAL_API */
        kCapContextUsageTypeCount = 14
    };

    U_I18N_API virtual UClassID getDynamicClassID() const override;

    U_I18N_API static UClassID getStaticClassID();

private:

    friend class SimpleDateFormat;
    friend class DateFormatSymbolsSingleSetter; 

    UnicodeString*  fEras;
    int32_t         fErasCount;

    UnicodeString*  fEraNames;
    int32_t         fEraNamesCount;

    UnicodeString*  fNarrowEras;
    int32_t         fNarrowErasCount;

    UnicodeString*  fMonths;
    int32_t         fMonthsCount;

    UnicodeString*  fShortMonths;
    int32_t         fShortMonthsCount;

    UnicodeString*  fNarrowMonths;
    int32_t         fNarrowMonthsCount;

    UnicodeString*  fStandaloneMonths;
    int32_t         fStandaloneMonthsCount;

    UnicodeString*  fStandaloneShortMonths;
    int32_t         fStandaloneShortMonthsCount;

    UnicodeString*  fStandaloneNarrowMonths;
    int32_t         fStandaloneNarrowMonthsCount;

    UnicodeString*  fWeekdays;
    int32_t         fWeekdaysCount;

    UnicodeString*  fShortWeekdays;
    int32_t         fShortWeekdaysCount;

    UnicodeString*  fShorterWeekdays;
    int32_t         fShorterWeekdaysCount;

    UnicodeString*  fNarrowWeekdays;
    int32_t         fNarrowWeekdaysCount;

    UnicodeString*  fStandaloneWeekdays;
    int32_t         fStandaloneWeekdaysCount;

    UnicodeString*  fStandaloneShortWeekdays;
    int32_t         fStandaloneShortWeekdaysCount;

    UnicodeString*  fStandaloneShorterWeekdays;
    int32_t         fStandaloneShorterWeekdaysCount;

    UnicodeString*  fStandaloneNarrowWeekdays;
    int32_t         fStandaloneNarrowWeekdaysCount;

    UnicodeString*  fAmPms;
    int32_t         fAmPmsCount;

    UnicodeString*  fWideAmPms;
    int32_t         fWideAmPmsCount;

    UnicodeString*  fNarrowAmPms;
    int32_t         fNarrowAmPmsCount;

    UnicodeString   fTimeSeparator;

    UnicodeString  *fQuarters;
    int32_t         fQuartersCount;

    UnicodeString  *fShortQuarters;
    int32_t         fShortQuartersCount;

    UnicodeString  *fNarrowQuarters;
    int32_t         fNarrowQuartersCount;
    
    UnicodeString  *fStandaloneQuarters;
    int32_t         fStandaloneQuartersCount;

    UnicodeString  *fStandaloneShortQuarters;
    int32_t         fStandaloneShortQuartersCount;

    UnicodeString  *fStandaloneNarrowQuarters;
    int32_t         fStandaloneNarrowQuartersCount;
    
    UnicodeString  *fLeapMonthPatterns;
    int32_t         fLeapMonthPatternsCount;

    UnicodeString  *fShortYearNames;
    int32_t         fShortYearNamesCount;

    UnicodeString  *fShortZodiacNames;
    int32_t         fShortZodiacNamesCount;

    UnicodeString   **fZoneStrings;         
    UnicodeString   **fLocaleZoneStrings;   
    int32_t         fZoneStringsRowCount;
    int32_t         fZoneStringsColCount;

    Locale                  fZSFLocale;         

    UnicodeString   fLocalPatternChars;

     UBool fCapitalization[kCapContextUsageTypeCount][2];

    UnicodeString  *fAbbreviatedDayPeriods;
    int32_t         fAbbreviatedDayPeriodsCount;

    UnicodeString  *fWideDayPeriods;
    int32_t         fWideDayPeriodsCount;

    UnicodeString  *fNarrowDayPeriods;
    int32_t         fNarrowDayPeriodsCount;

    UnicodeString  *fStandaloneAbbreviatedDayPeriods;
    int32_t         fStandaloneAbbreviatedDayPeriodsCount;

    UnicodeString  *fStandaloneWideDayPeriods;
    int32_t         fStandaloneWideDayPeriodsCount;

    UnicodeString  *fStandaloneNarrowDayPeriods;
    int32_t         fStandaloneNarrowDayPeriodsCount;

private:
    Locale validLocale;
    Locale actualLocale;

    DateFormatSymbols() = delete; 

    void initializeData(const Locale& locale, const char *type,
                        UErrorCode& status, UBool useLastResortData = false);

    static void assignArray(UnicodeString*& dstArray,
                            int32_t& dstCount,
                            const UnicodeString* srcArray,
                            int32_t srcCount);

    static UBool arrayCompare(const UnicodeString* array1,
                             const UnicodeString* array2,
                             int32_t count);

    void createZoneStrings(const UnicodeString *const * otherStrings);

    void dispose();

    void copyData(const DateFormatSymbols& other);

    void initZoneStringsArray();

    void disposeZoneStrings();

    static UDateFormatField U_EXPORT2 getPatternCharIndex(char16_t c);

    static UBool U_EXPORT2 isNumericField(UDateFormatField f, int32_t count);

    static UBool U_EXPORT2 isNumericPatternChar(char16_t c, int32_t count);
public:
#ifndef U_HIDE_INTERNAL_API
  U_I18N_API static DateFormatSymbols* createForLocale(const Locale& locale, UErrorCode& status);
#endif  /* U_HIDE_INTERNAL_API */
};

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */

#endif /* U_SHOW_CPLUSPLUS_API */

#endif // _DTFMTSYM
