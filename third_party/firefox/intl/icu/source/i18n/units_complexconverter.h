// License & terms of use: http://www.unicode.org/copyright.html

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING
#ifndef __UNITS_COMPLEXCONVERTER_H__
#define __UNITS_COMPLEXCONVERTER_H__

#include "cmemory.h"
#include "measunit_impl.h"
#include "number_roundingutils.h"
#include "unicode/errorcode.h"
#include "unicode/measure.h"
#include "units_converter.h"
#include "units_data.h"

U_NAMESPACE_BEGIN

namespace units {

class U_I18N_API_CLASS ComplexUnitsConverter : public UMemory {
  public:
    ComplexUnitsConverter(const MeasureUnitImpl &targetUnit, const ConversionRates &ratesInfo,
                          UErrorCode &status);
    U_I18N_API ComplexUnitsConverter(StringPiece inputUnitIdentifier,
                                     StringPiece outputUnitsIdentifier,
                                     UErrorCode &status);

    U_I18N_API ComplexUnitsConverter(const MeasureUnitImpl &inputUnit,
                                     const MeasureUnitImpl &outputUnits,
                                     const ConversionRates &ratesInfo,
                                     UErrorCode &status);

    UBool greaterThanOrEqual(double quantity, double limit) const;

    U_I18N_API MaybeStackVector<Measure>
    convert(double quantity, icu::number::impl::RoundingImpl *rounder, UErrorCode &status) const;

    MaybeStackVector<UnitsConverter> unitsConverters_;

    MaybeStackVector<MeasureUnitImplWithIndex> units_;

  private:
    void init(const MeasureUnitImpl &inputUnit, const ConversionRates &ratesInfo, UErrorCode &status);

    void applyRounder(MaybeStackArray<int64_t, 5> &intValues, double &quantity,
                      icu::number::impl::RoundingImpl *rounder, UErrorCode &status) const;
};

} 
U_NAMESPACE_END

#endif //__UNITS_COMPLEXCONVERTER_H__

#endif /* #if !UCONFIG_NO_FORMATTING */
