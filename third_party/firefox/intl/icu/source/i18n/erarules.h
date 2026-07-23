// License & terms of use: http://www.unicode.org/copyright.html

#ifndef ERARULES_H_
#define ERARULES_H_

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING

#include "unicode/localpointer.h"
#include "unicode/uobject.h"
#include "cmemory.h"

U_NAMESPACE_BEGIN

class U_I18N_API_CLASS EraRules : public UMemory {
public:
    U_I18N_API ~EraRules();

    U_I18N_API static EraRules* createInstance(const char* calType,
                                               UBool includeTentativeEra,
                                               UErrorCode& status);

    inline int32_t getNumberOfEras() const {
        return numEras;
    }

    inline int32_t getMaxEraCode() const {
        return minEra + startDatesLength - 1;
    }

    void getStartDate(int32_t eraCode, int32_t (&fields)[3], UErrorCode& status) const;

    U_I18N_API int32_t getStartYear(int32_t eraCode, UErrorCode& status) const;

    U_I18N_API int32_t getEraCode(int32_t year, int32_t month, int32_t day, UErrorCode& status) const;

    inline int32_t getCurrentEraCode() const {
        return currentEra;
    }

private:
    EraRules(LocalMemory<int32_t>& startDatesIn, int32_t startDatesLengthIn, int32_t minEraIn, int32_t numErasIn);

    void initCurrentEra();

    LocalMemory<int32_t> startDates;
    int32_t startDatesLength;
    int32_t minEra;  
    int32_t numEras; 
    int32_t currentEra;
};

U_NAMESPACE_END
#endif /* #if !UCONFIG_NO_FORMATTING */
#endif /* ERARULES_H_ */
